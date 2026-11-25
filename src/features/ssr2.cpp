// Our screen-space reflections for every glossy surface. The shaders are in shaders/ssr2.hlsl (based on AMD
// FidelityFX SSSR, MIT license). The passes, per frame, after the scene hook's shared ones:
//   trace (one GGX ray per pixel over the depth pyramid, colour from last frame's capture)
//   -> the shared temporal accumulation -> spatial resolve -> (half scale) depth-aware upsample
//   -> the texture the patched lighting reads at space-2 t14.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "ssr2.h"
#include "light.h"
#include "light_patch.h"
#include "log.h"
#include "scene_inject.h"

#include "CSSpatial.h"
#include "CSTrace.h"
#include "CSUpsample.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// key, label, tooltip, Game's value, min, max, format, integer, logarithmic, advanced (not shown)
const param_def MAT_DEFS[MAT_PARAMS] = {
    {"Strength", "Strength",
     "How much the screen-space reflection replaces the game's. The game uses 0.", 0.0f, 0.0f,
     1.0f, "%.2f", false, false, false},
    {"MaxRoughness", "Max roughness", "Rougher surfaces keep the game's reflection.", 0.55f, 0.3f,
     0.8f, "%.2f", false, false, false},
    {"RaySteps", "Ray steps", "How far and how finely each ray searches. More costs more.", 64.0f, 32.0f,
     128.0f, "%.0f", true, false, false},
    {"Resolution", "Resolution", "Half, three quarters or full. Higher is sharper and costs more.", 0.5f,
     0.5f, 1.0f, "%.1f", false, false, false},
    {"Fade", "Roughness fade", nullptr, 0.3f, 0.01f, 1.0f, "%.2f", false, false, true},
    {"Thickness", "Thickness", nullptr, 0.3f, 0.01f, 10.0f, "%.2f", false, true, true},
    {"Trim", "Ray spread", nullptr, 0.7f, 0.0f, 1.0f, "%.2f", false, false, true},
    {"Radius", "Blur radius", nullptr, 8.0f, 0.0f, 32.0f, "%.1f", false, false, true},
    {"Cap", "Brightness cap", nullptr, 32.0f, 1.0f, 1024.0f, "%.0f", false, true, true},
    {"Blend", "History blend", nullptr, 0.08f, 0.01f, 1.0f, "%.2f", false, false, true},
    {"Length", "History length", nullptr, 16.0f, 1.0f, 64.0f, "%.0f", true, false, true},
    {"MoveLow", "Motion fade start", nullptr, 1.0f, 0.0f, 32.0f, "%.1f", false, false, true},
    {"MoveHigh", "Motion fade end", nullptr, 4.0f, 0.0f, 64.0f, "%.1f", false, false, true},
    {"Characters", "Characters reflect", nullptr, 1.0f, 0.0f, 1.0f, "%.0f", true, false, true},
    {"CharacterHits", "Reflect characters", nullptr, 1.0f, 0.0f, 1.0f, "%.0f", true, false, true},
};

// Game is the game's own probe reflection (Strength 0).
const float MAT_PRESET_VALUES[PRESETS][MAT_PARAMS] = {
    {0.0f, 0.55f, 64, 0.5f, 0.3f, 0.3f, 0.7f, 8, 32, 0.08f, 16, 1, 4, 1, 1},
    {0.6f, 0.55f, 48, 0.5f, 0.3f, 0.3f, 0.7f, 8, 32, 0.08f, 16, 1, 4, 1, 1},
    {0.8f, 0.7f, 64, 0.75f, 0.3f, 0.3f, 0.7f, 8, 32, 0.08f, 16, 1, 4, 1, 1},
    {1.0f, 0.7f, 96, 1.0f, 0.3f, 0.3f, 0.7f, 8, 32, 0.08f, 16, 1, 4, 1, 1},
};

namespace
{
const uint32_t FLAG_FIRST = 1, FLAG_FINAL = 16, FLAG_CHARACTERS = 128, FLAG_NO_CHARACTER_HITS = 256;

float g_values[MAT_PARAMS];
bool g_on = false;      // the pass runs (Strength > 0 once started)
bool g_started = false; // the game has drawn a while: values take effect

scene::texture g_trace, g_history[2], g_length[2], g_resolved, g_output;

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
    // Half or three quarters: a smaller trace, then the upsample.
    const float scale = v(MAT_SCALE);
    const bool half = scale < 1.0f;
    const uint32_t tw = half ? std::max(uint32_t(std::lround(f.width * scale)), 1u) : f.width,
                   th = half ? std::max(uint32_t(std::lround(f.height * scale)), 1u) : f.height;
    const uint32_t depth_mip = scale < 0.6f ? 1 : 0;
    bool fresh = false;
    fresh |= scene::make_texture(g_trace, DXGI_FORMAT_R16G16B16A16_FLOAT, tw, th, 1, false);
    for (int i = 0; i < 2; ++i)
    {
        fresh |= scene::make_texture(g_history[i], DXGI_FORMAT_R16G16B16A16_FLOAT, tw, th, 1, false);
        fresh |= scene::make_texture(g_length[i], DXGI_FORMAT_R16_FLOAT, tw, th, 1, false);
    }
    if (half)
        fresh |= scene::make_texture(g_resolved, DXGI_FORMAT_R16G16B16A16_FLOAT, tw, th, 1, false);
    scene::make_texture(g_output, DXGI_FORMAT_R16G16B16A16_FLOAT, f.width, f.height, 1, true);
    scene::set_output(0, &g_output);

    scene::frame fr = f;
    if (fresh)
        fr.flags |= FLAG_FIRST;
    using scene::view;
    fr.flags |= (v(MAT_CHARACTERS) >= 0.5f ? FLAG_CHARACTERS : 0) |
                (v(MAT_CHARACTER_HITS) < 0.5f ? FLAG_NO_CHARACTER_HITS : 0);
    const uint32_t gx = (tw + 7) / 8, gy = (th + 7) / 8;

    uint32_t c[16] = {tw, th, f.width, f.height, f2u(v(MAT_MAX_ROUGH)), f2u(v(MAT_THICKNESS)), f2u(v(MAT_FADE)),
                      f2u(v(MAT_TRIM)), uint32_t(fr.run), fr.flags, f.pyramid_mips,
                      uint32_t(std::clamp(v(MAT_STEPS), 8.0f, 512.0f)), f2u(v(MAT_CAP)), f2u(v(MAT_MOVE_LOW)),
                      f2u(v(MAT_MOVE_HIGH))};
    scene::dispatch(fr, scene::pipeline(CSTrace, sizeof(CSTrace)),
                    {view::of(*s.pyramid), view::of(*s.normals), view::of(*s.depth_now), view::of(*s.capture),
                     view::from(scene::INPUT_ALBEDO), view::from(scene::INPUT_MATERIAL), view::of(*s.motion)},
                    {view::of(g_trace, 0)}, c, gx, gy);

    scene::texture &now = g_history[fr.run & 1], &before = g_history[(fr.run + 1) & 1];
    scene::texture &len_now = g_length[fr.run & 1], &len_before = g_length[(fr.run + 1) & 1];
    scene::temporal(fr, s, g_trace, before, now, len_before, len_now, depth_mip, v(MAT_BLEND), v(MAT_LENGTH));

    uint32_t r[16] = {tw, th, f.width, f.height, f2u(v(MAT_RADIUS)), 0, 0, 0, uint32_t(fr.run),
                      fr.flags | (half ? 0 : FLAG_FINAL), depth_mip, 0, f2u(v(MAT_STRENGTH))};
    scene::dispatch(fr, scene::pipeline(CSSpatial, sizeof(CSSpatial)),
                    {view::of(*s.pyramid), view::of(*s.normals), view::of(*s.depth_now), view::of(*s.capture),
                     view::from(scene::INPUT_ALBEDO), view::from(scene::INPUT_MATERIAL), view::of(now)},
                    {view::of(half ? g_resolved : g_output, 0)}, r, gx, gy);
    if (half)
    {
        uint32_t u[16] = {f.width, f.height, tw, th, 0, 0, 0, 0, uint32_t(fr.run), fr.flags | FLAG_FINAL,
                          depth_mip, 0, f2u(v(MAT_STRENGTH))};
        scene::dispatch(fr, scene::pipeline(CSUpsample, sizeof(CSUpsample)),
                        {view::of(*s.pyramid), view::of(*s.normals), view::of(*s.depth_now), view::of(*s.capture),
                         view::from(scene::INPUT_ALBEDO), view::from(scene::INPUT_MATERIAL), view::of(g_resolved)},
                        {view::of(g_output, 0)}, u, (f.width + 7) / 8, (f.height + 7) / 8);
    }
}

void turn(bool on)
{
    if (on == g_on)
        return;
    g_on = on;
    scene::set_output(0, on ? &g_output : nullptr);
    // The gloss export goes on once the scene hook has seen u0 in the lighting's space-2 table.
    light_set_ssr2(on ? LIGHT_SSR2_REGISTER : 0, false);
    scene::enable(on);
    dd_log(reshade::log::level::info, on ? "Surface reflections: on" : "Surface reflections: off");
}
} // namespace

void ssr2_set(const float values[MAT_PARAMS])
{
    for (int i = 0; i < MAT_PARAMS; ++i)
    {
        g_values[i] = std::clamp(values[i], MAT_DEFS[i].min, MAT_DEFS[i].max);
        if (MAT_DEFS[i].integer)
            g_values[i] = std::round(g_values[i]);
    }
    g_values[MAT_SCALE] = g_values[MAT_SCALE] < 0.625f ? 0.5f : g_values[MAT_SCALE] < 0.875f ? 0.75f : 1.0f;
    if (g_started)
        turn(g_values[MAT_STRENGTH] > 0.0f);
}

void ssr2_start()
{
    g_started = true;
    turn(g_values[MAT_STRENGTH] > 0.0f);
}

void ssr2_register()
{
    scene::add_pass(pass);
}
