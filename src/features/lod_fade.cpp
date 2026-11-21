// Steam build 26142239:
// camera = [[exe+0x60A6128]+0x60], table = [camera+0x9D0], component = [table + 8 * dword exe+0x60A37DC].
// Component +0x14 switch band (m), +0x18 cross-fade time (s), +0x20 / +0x24 the fast-camera speeds (40 / 50 m/s).
// Reads go through ReadProcessMemory so a stale pointer fails instead of faulting. Nothing is written unless the
// fast-camera speeds hold the game's 40 and 50. This mod never changes them.
// Tested while riding (2026-11-20): 1.5 s made pop-in more visible. 0.25 s looked best. 0 snaps.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "lod_fade.h"

#include <cstdint>

namespace
{
const uintptr_t CAMERA_GLOBAL = 0x60A6128;
const uintptr_t INDEX_GLOBAL = 0x60A37DC;
const unsigned CAMERA = 0x60, TABLE = 0x9D0, BAND = 0x14, FADE = 0x18, SPEED_LOW = 0x20, SPEED_HIGH = 0x24;

float g_fade = 0.0f;
float g_wrote = 0.0f; // the fade we wrote last, 0 when the game's own is in place
float g_game_fade = -1.0f;

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

uintptr_t find()
{
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t global = 0, camera = 0, table = 0, component = 0;
    uint32_t index = 0;
    if (!read(exe + CAMERA_GLOBAL, global) || !read(global + CAMERA, camera) || !read(camera + TABLE, table) ||
        !read(exe + INDEX_GLOBAL, index) || index > 64 || !read(table + 8 * uintptr_t(index), component))
        return 0;
    float band = 0.0f, fade = 0.0f, low = 0.0f, high = 0.0f;
    if (!read(component + BAND, band) || !read(component + FADE, fade) || !read(component + SPEED_LOW, low) ||
        !read(component + SPEED_HIGH, high))
        return 0;
    if (low != 40.0f || high != 50.0f || !(band >= 0.0f && band < 100.0f) || !(fade >= 0.0f && fade < 30.0f))
        return 0;
    return component;
}
} // namespace

void lod_fade_set(float fade_seconds)
{
    g_fade = fade_seconds;
}

void lod_fade_frame()
{
    static unsigned counter = 0;
    if (++counter % 30 != 0)
        return;
    const uintptr_t component = find();
    if (component == 0)
        return;
    float fade = 0.0f;
    read(component + FADE, fade);
    // The first component found holds the game's own values.
    if (g_game_fade < 0.0f)
        g_game_fade = fade;
    // At 0 the game's own value stays. Only a value we wrote is put back. The game may change it freely.
    if (g_fade > 0.0f)
    {
        if (fade != g_fade)
        {
            write(component + FADE, g_fade);
            g_wrote = g_fade;
        }
    }
    else if (g_wrote > 0.0f)
    {
        if (fade == g_wrote)
            write(component + FADE, g_game_fade);
        g_wrote = 0.0f;
    }
}

float lod_fade_game_fade()
{
    return g_game_fade;
}

