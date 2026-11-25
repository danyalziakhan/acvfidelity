// Screen-space indirect light. The shaders are in shaders/ssil.hlsl. The passes, per frame, after the scene hook's
// shared ones:
//   trace (bitmask slices over linear depth, colour from last frame's capture at newly occluded sectors)
//   -> our own temporal accumulation (no history clamp) -> a-trous passes -> final (upsample, strength)
//   -> the texture the patched lighting reads at space-2 t15.
//
// No double count: the game's AO already removes the probe bounce and sky light in the occluded part of the
// hemisphere (its AO multiplies both). Our light comes only from the sectors the trace finds occluded. So it refills
// that part and adds nothing where our visibility is 1. Our visibility replaces the game's screen AO in the game's
// own occlusion of its bounce light (light_patch.h).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "ssil.h"
#include "light.h"
#include "light_patch.h"
#include "log.h"
#include "scene_inject.h"

#include "SSILFinal.h"
#include "SSILSpatial.h"
#include "SSILTemporal.h"
#include "SSILTrace.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// key, label, tooltip, Game's value, min, max, format, integer, logarithmic, advanced (not shown)
const param_def SSIL_DEFS[SSIL_PARAMS] = {
    {"Strength", "Strength",
     "How strongly nearby surfaces light each other. The game uses 0 (off).\n"
     "It also takes over the game's screen-space AO in shade.",
     0.0f, 0.0f, 10.0f, "%.2f", false, false, false},
    // The thickness follows the ambient occlusion section: one visibility for the whole hemisphere.
    {"Radius", "Radius", "How far a surface may be to light a spot. More costs more.", 2.0f, 0.25f,
     8.0f, "%.2f m", false, false, false},
    {"Steps", "Steps", "Samples along each direction. More gives steadier light and costs more.", 12.0f, 2.0f, 32.0f, "%.0f", true,
     false, false},
    {"Directions", "Directions", "Directions each pixel searches. More gives steadier light and costs more.", 2.0f,
     1.0f, 8.0f, "%.0f", true, false, false},
    {"Scale", "Resolution", nullptr, 0.5f, 0.5f, 1.0f, "%.2f", false, false, true},
    {"History", "History length", nullptr, 16.0f, 1.0f, 64.0f, "%.0f", true, false, true},
    {"Cap", "Brightness cap", nullptr, 8.0f, 0.1f, 1000.0f, "%.1f", false, false, true},
    {"Blend", "History blend", nullptr, 0.05f, 0.01f, 1.0f, "%.2f", false, false, true},
    {"Falloff", "Falloff start", nullptr, 0.7f, 0.0f, 1.0f, "%.2f", false, false, true},
    {"Spread", "Step spread", nullptr, 1.4f, 1.0f, 3.0f, "%.2f", false, false, true},
    {"Facing", "Back-face band", nullptr, 0.1f, 0.0f, 1.0f, "%.2f", false, false, true},
    {"Passes", "Blur passes", nullptr, 2.0f, 0.0f, 3.0f, "%.0f", true, false, true},
    {"MaxPixels", "Max radius", nullptr, 256.0f, 16.0f, 1024.0f, "%.0f px", false, false, true},
    {"DepthTolerance", "Blur depth tolerance", nullptr, 0.02f, 0.005f, 0.2f, "%.3f", false, false, true},
    {"NormalPower", "Blur normal power", nullptr, 8.0f, 1.0f, 64.0f, "%.0f", false, false, true},
};

// Game is the game's own ambient (Strength 0).
const float SSIL_PRESET_VALUES[PRESETS][SSIL_PARAMS] = {
    {0.0f, 2.0f, 12, 2, 0.5f, 16, 8, 0.05f, 0.7f, 1.4f, 0.1f, 2, 256, 0.02f, 8},
    {2.8f, 2.0f, 12, 2, 0.5f, 16, 8, 0.05f, 0.7f, 1.4f, 0.1f, 2, 256, 0.02f, 8},
    {2.3f, 3.0f, 20, 2, 0.5f, 16, 8, 0.05f, 0.7f, 1.4f, 0.1f, 2, 256, 0.02f, 8},
    {2.0f, 4.0f, 32, 2, 0.5f, 16, 8, 0.05f, 0.7f, 1.4f, 0.1f, 2, 256, 0.02f, 8},
};

namespace
{
const uint32_t FLAG_FIRST = 1, FLAG_HALF = 524288;

float g_values[SSIL_PARAMS];
float g_thickness = 0.35f;
bool g_on = false;
bool g_started = false;

scene::texture g_trace, g_history[2], g_length[2], g_ping[2], g_output;

uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

float v(int i)
{
    return g_values[i];
}

void pass(const scene::frame &f, const scene::shared_set &s)
{
    if (!g_on)
        return;
    const bool half = v(SSIL_SCALE) < 0.75f;
    const uint32_t tw = half ? std::max(f.width / 2, 1u) : f.width, th = half ? std::max(f.height / 2, 1u) : f.height;
    const uint32_t depth_mip = half ? 1 : 0;
    bool fresh = false;
    fresh |= scene::make_texture(g_trace, DXGI_FORMAT_R16G16B16A16_FLOAT, tw, th, 1, false);
    for (int i = 0; i < 2; ++i)
    {
        fresh |= scene::make_texture(g_history[i], DXGI_FORMAT_R16G16B16A16_FLOAT, tw, th, 1, false);
        fresh |= scene::make_texture(g_length[i], DXGI_FORMAT_R16_FLOAT, tw, th, 1, false);
        scene::make_texture(g_ping[i], DXGI_FORMAT_R16G16B16A16_FLOAT, tw, th, 1, false);
    }
    scene::make_texture(g_output, DXGI_FORMAT_R16G16B16A16_FLOAT, f.width, f.height, 1, true);
    scene::set_output(1, &g_output);

    scene::frame fr = f;
    if (fresh)
        fr.flags |= FLAG_FIRST;
    using scene::view;
    const uint32_t slices = uint32_t(v(SSIL_SLICES)), steps = uint32_t(v(SSIL_STEPS));
    uint32_t c[16] = {tw, th, f.width, f.height, f2u(v(SSIL_RADIUS)), f2u(g_thickness), f2u(v(SSIL_FALLOFF)),
                      f2u(v(SSIL_SPREAD)), uint32_t(fr.run), fr.flags, depth_mip, slices | steps << 16,
                      f2u(v(SSIL_CAP)), f2u(v(SSIL_FACING)), f2u(v(SSIL_MAX_PIXELS))};
    const uint32_t gx = (tw + 7) / 8, gy = (th + 7) / 8;
    scene::dispatch(fr, scene::pipeline(SSILTrace, sizeof(SSILTrace)),
                    {view::of(*s.depth_now), view::of(*s.normals), view::of(*s.capture), view::of(*s.motion)},
                    {view::of(g_trace, 0), view()}, c, gx, gy);

    // Our own accumulation without a history clamp (shaders/ssil.hlsl SSILTemporal). A clamp to this frame's
    // neighbors pulled sparse bright history to 0.
    scene::texture &now = g_history[fr.run & 1], &before = g_history[(fr.run + 1) & 1];
    scene::texture &len_now = g_length[fr.run & 1], &len_before = g_length[(fr.run + 1) & 1];
    {
        uint32_t t[16] = {tw, th, f.width, f.height, f2u(v(SSIL_BLEND)), f2u(v(SSIL_HISTORY)), 0, 0, uint32_t(fr.run),
                          fr.flags, depth_mip};
        scene::dispatch(fr, scene::pipeline(SSILTemporal, sizeof(SSILTemporal)),
                        {view::of(g_trace), view::of(*s.depth_now), view(), view::of(*s.motion), view::of(before),
                         view::of(len_before), view::of(*s.depth_before)},
                        {view::of(now, 0), view(), view::of(len_now, 0)}, t, gx, gy);
    }

    scene::texture *src = &now;
    for (int i = 0; i < int(v(SSIL_PASSES)); ++i)
    {
        scene::texture &dst = g_ping[i & 1];
        uint32_t p[16] = {tw, th, f.width, f.height, f2u(float(1 << i)), f2u(v(SSIL_DEPTH_TOLERANCE)),
                          f2u(v(SSIL_NORMAL_POWER)), 0, uint32_t(fr.run), fr.flags, depth_mip};
        scene::dispatch(fr, scene::pipeline(SSILSpatial, sizeof(SSILSpatial)),
                        {view::of(*src), view::of(*s.depth_now), view::of(*s.normals)}, {view::of(dst, 0)}, p, gx, gy);
        src = &dst;
    }

    uint32_t u[16] = {f.width, f.height, tw, th, f2u(v(SSIL_STRENGTH)), 0, 0, 0, uint32_t(fr.run),
                      fr.flags | (half ? FLAG_HALF : 0)};
    scene::dispatch(fr, scene::pipeline(SSILFinal, sizeof(SSILFinal)),
                    {view::of(*src), view::of(*s.depth_now), view::of(*s.normals)}, {view::of(g_output, 0)}, u,
                    (f.width + 7) / 8, (f.height + 7) / 8);
}

void turn(bool on)
{
    if (on == g_on)
        return;
    g_on = on;
    scene::set_output(1, on ? &g_output : nullptr);
    light_set_ssil(on ? LIGHT_SSIL_REGISTER : 0);
    scene::enable_for(scene::USER_SSIL, on);
    dd_log(reshade::log::level::info, on ? "Indirect lighting: on" : "Indirect lighting: off");
}
} // namespace

void ssil_set(const float values[SSIL_PARAMS], float thickness)
{
    for (int i = 0; i < SSIL_PARAMS; ++i)
    {
        g_values[i] = std::clamp(values[i], SSIL_DEFS[i].min, SSIL_DEFS[i].max);
        if (SSIL_DEFS[i].integer)
            g_values[i] = std::round(g_values[i]);
    }
    g_values[SSIL_SCALE] = g_values[SSIL_SCALE] < 0.75f ? 0.5f : 1.0f;
    g_thickness = std::clamp(thickness, 0.01f, 2.0f);
    if (g_started)
        turn(g_values[SSIL_STRENGTH] > 0.0f);
}

void ssil_start()
{
    g_started = true;
    turn(g_values[SSIL_STRENGTH] > 0.0f);
}

void ssil_register()
{
    scene::add_pass(pass);
}
