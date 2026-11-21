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

#include <cmath>

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

void ssr_angle_frame(uintptr_t lod_object)
{
    if (++g_counter % 30 != 0)
        return;
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

