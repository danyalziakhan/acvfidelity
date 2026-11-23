// Steam build 26142239. The path to the water settings, as the game's own SSR code takes it:
//   world    = [[[object + 0x7240] + 0x248]], valid while bit 31 of the dword at handle + 0x8 is set
//   river    = [world + 0x1B38], the river world component (0x7FFF... while the game hasn't looked it up yet)
//   data     = handle [river + 0x70], the component's data
//   settings = handle [data + 0x78]
// Each frame exe+0xAC1480 writes the fade at settings + 0x14C:
//   fade = clamp((down - [+0x44]) / ([+0x40] - [+0x44]), 0, 1), down = the view direction's downward part,
// and the SSR pass is skipped at 0 (exe+0x17505D8). The game's +0x40 / +0x44 are 0.42 / 0.57: full reflections up
// to about 25 degrees below level, none past 35. Tested 2026-11-21: 0.80 / 0.95 (53 / 72 degrees) looked right
// while tilting down over water. Measured the same day at a shore: reflections cover 17% of the image at 6
// degrees, 7% at 25 and under 0.5% from 32 on. The rays leave the screen. So a wider angle only pays in views
// over open water with tall things across it, like the raven over a lake.
// Reads and writes go through ReadProcessMemory / WriteProcessMemory. A stale pointer then fails instead of
// faulting. Nothing is written unless the two floats hold the game's values or ours.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ssr_angle.h"

#include <reshade.hpp>

#include "log.h"

#include <atomic>
#include <cmath>
#include <cstdio>

namespace
{
const uint32_t TO_SCENE = 0x7240, TO_HANDLE = 0x248, HANDLE_FLAGS = 0x8;
const uint32_t RIVER = 0x1B38, DATA = 0x70, SETTINGS = 0x78, FULL = 0x40, NONE = 0x44;
const float GAME_FULL = 0.42f, GAME_NONE = 0.57f;
const float GAME_DEGREES = 35.0f;
// The fade starts at this share of the end angle (the game's 25 of 35, the tested 52 of 72).
const float START_SHARE = 0.72f;

float g_full = GAME_FULL, g_none = GAME_NONE;     // what we want there now
float g_wrote_full = 0.0f, g_wrote_none = 0.0f; // what we wrote last
bool g_wrote = false;
unsigned g_counter = 0;
uintptr_t g_object = 0; // the settings object found last

template <typename T> bool read(uintptr_t address, T &value)
{
    SIZE_T got = 0;
    return address != 0 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), &value, sizeof(T), &got) &&
           got == sizeof(T);
}

void write(uintptr_t address, float value)
{
    SIZE_T done = 0;
    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), &value, sizeof(value), &done);
}

// The game's handle: the pointer at h, valid while bit 31 of the dword at h + 8 is set.
uintptr_t handle(uintptr_t h)
{
    uint32_t flags = 0;
    uintptr_t value = 0;
    if (!read(h + HANDLE_FLAGS, flags) || (flags & 0x80000000u) == 0 || !read(h, value))
        return 0;
    return value;
}

uintptr_t settings_of(uintptr_t object)
{
    uintptr_t scene = 0, h = 0, river = 0;
    if (object == 0 || !read(object + TO_SCENE, scene) || !read(scene + TO_HANDLE, h))
        return 0;
    const uintptr_t world = handle(h);
    if (world == 0 || !read(world + RIVER, river) || river == 0 || river == 0x7FFFFFFFFFFFFFFFull ||
        !read(river + DATA, h))
        return 0;
    const uintptr_t data = handle(h);
    if (data == 0 || !read(data + SETTINGS, h))
        return 0;
    return handle(h);
}

bool close_to(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}
} // namespace

void ssr_angle_set(float degrees)
{
    if (!(degrees > GAME_DEGREES) || degrees > 85.0f)
    {
        g_full = GAME_FULL;
        g_none = GAME_NONE;
        return;
    }
    const float to_radians = 3.14159265f / 180.0f;
    g_full = std::sin(degrees * START_SHARE * to_radians);
    g_none = std::sin(degrees * to_radians);
}

namespace
{
// The SSR resolution. Each frame exe+0x17501F0 sets GfxWaterOptions +0x84 / +0x88
// (the SSR target) to scale(enum, base size) through exe+0xA513D0: 0 full, 1 three quarters, 2 half (the result
// truncated to whole pixels, at most the base size). The enum is per platform in
// the river component's resolution object, handle [river + 0x78]. This build reads +0x48 (platform 7, level 2).
// The game's value there is 2. The trace, its prepare and resolve, the targets and their constants all follow.
const uint32_t RESOLUTION = 0x78, RESOLUTION_ENUM = 0x48;
const int32_t ENUM_HALF = 2;
const int32_t ENUM_OF_MODE[3] = {2, 1, 0}; // the setting: half, three quarters, full
// GfxWaterOptions = [exe + OPTIONS + 16 x [exe + WATER_INDEX]], the render size options the same way.
const uintptr_t OPTIONS = 0x62D18D8, WATER_INDEX = 0x62F18AC, RENDER_INDEX = 0x6097D0C;
const uint32_t SSR_WIDTH = 0x84, RENDER_WIDTH = 0x30;

int g_res_mode = 0;                    // what the setting asks for: 0 = half, 1 = three quarters, 2 = full
std::atomic<int> g_res_live{-1};       // the live target: 0 = half, 1 = three quarters, 2 = full, -1 other
uintptr_t g_res_object = 0;            // where we wrote last
bool g_res_wrote = false;              // our value is there now
int32_t g_res_last = 0;                // the value we wrote last
uint32_t g_ssr_width = 0, g_render_width = 0;

void write_i32(uintptr_t address, int32_t value)
{
    SIZE_T done = 0;
    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), &value, sizeof(value), &done);
}

uintptr_t river_of(uintptr_t object)
{
    uintptr_t scene = 0, h = 0, river = 0;
    if (object == 0 || !read(object + TO_SCENE, scene) || !read(scene + TO_HANDLE, h))
        return 0;
    const uintptr_t world = handle(h);
    if (world == 0 || !read(world + RIVER, river) || river == 0x7FFFFFFFFFFFFFFFull)
        return 0;
    return river;
}

uintptr_t options(uintptr_t base, uintptr_t index_at)
{
    uint32_t index = 0;
    uintptr_t object = 0;
    if (!read(base + index_at, index) || index > 255 || !read(base + OPTIONS + 16 * uintptr_t(index), object))
        return 0;
    return object;
}

void res_frame(uintptr_t lod_object)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint32_t ssr = 0, render = 0;
    read(options(base, WATER_INDEX) + SSR_WIDTH, ssr);
    read(options(base, RENDER_INDEX) + RENDER_WIDTH, render);
    if (ssr != g_ssr_width || render != g_render_width)
    {
        char text[128];
        snprintf(text, sizeof(text), "SSR: the reflection target is %u wide, the render %u", ssr, render);
        dd_log(reshade::log::level::info, text);
    }
    g_ssr_width = ssr;
    g_render_width = render;
    // The target's width against the render's, as the game rounds it (truncated): which upscale fits.
    const int live = ssr == 0 || render == 0           ? -1
                     : ssr == render                     ? 2
                     : ssr == render * 3 / 4             ? 1
                     : ssr == render / 2                 ? 0
                                                         : -1;
    g_res_live = live;

    uintptr_t h = 0, object = 0;
    const uintptr_t river = river_of(lod_object);
    if (river == 0 || !read(river + RESOLUTION, h) || (object = handle(h)) == 0)
        return;
    if (object != g_res_object)
        g_res_wrote = false; // a new world: the game's value again
    g_res_object = object;
    int32_t now = -1;
    if (!read(object + RESOLUTION_ENUM, now))
        return;
    const int32_t want = ENUM_OF_MODE[g_res_mode];
    if (now == want)
        return;
    // Only over the game's value or the one we wrote last.
    if (now != ENUM_HALF && !(g_res_wrote && now == g_res_last))
        return;
    write_i32(object + RESOLUTION_ENUM, want);
    g_res_wrote = want != ENUM_HALF;
    g_res_last = want;
    static const char *const NAMES[3] = {"half", "three quarters", "full"};
    char text[128];
    snprintf(text, sizeof(text), "SSR: water reflections resolution set to %s (0x%llX + 0x48 = %d)",
             NAMES[g_res_mode], static_cast<unsigned long long>(object), want);
    dd_log(reshade::log::level::info, text);
}
} // namespace

void ssr_res_set(int mode)
{
    g_res_mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
}

void ssr_res_restore()
{
    int32_t now = -1;
    if (g_res_wrote && g_res_object != 0 && read(g_res_object + RESOLUTION_ENUM, now) && now == g_res_last)
        write_i32(g_res_object + RESOLUTION_ENUM, ENUM_HALF);
    g_res_wrote = false;
}

int ssr_res_live()
{
    return g_res_live.load();
}

void ssr_angle_frame(uintptr_t lod_object)
{
    if (++g_counter % 30 != 0)
        return;
    res_frame(lod_object);
    const uintptr_t s = settings_of(lod_object);
    g_object = s;
    float full = 0.0f, none = 0.0f;
    if (s == 0 || !read(s + FULL, full) || !read(s + NONE, none))
        return;
    if (full == g_full && none == g_none)
        return;
    // Only over the game's values or the ones we wrote: anything else means this is not the object we know.
    const bool known = (close_to(full, GAME_FULL) && close_to(none, GAME_NONE)) ||
                       (g_wrote && close_to(full, g_wrote_full) && close_to(none, g_wrote_none));
    if (!known)
        return;
    write(s + FULL, g_full);
    write(s + NONE, g_none);
    g_wrote_full = g_full;
    g_wrote_none = g_none;
    g_wrote = true;
}

