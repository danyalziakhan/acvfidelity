// The path to the grids, from the LOD object (Steam build 26142239):
//   world  = [[[object + 0x7240] + 0x248]], valid while bit 31 of the dword at handle + 0x8 is set
//   grids  = [world + 0x17d0 + 8 * i] for i = 0..3, then each grid's next one at +0x48 (both world layers)
//   kind   = [[grid + 0x308] + 0x154]: 0 MainGrid, 1 ShortRangeGrid, 2 LongRangeGrid, 3 FakeGrid
//   range  = float at grid + 0x198, 1.0 unless changed. The load range is base x factor x this.
// Every read goes through ReadProcessMemory. A stale pointer then fails the read instead of faulting. Nothing
// is written unless every step looks as expected.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "grass.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace
{
const uint32_t TO_SCENE = 0x7240;
const uint32_t TO_HANDLE = 0x248;
const uint32_t HANDLE_FLAGS = 0x8;
const uint32_t GRIDS = 0x17d0;
const uint32_t NEXT_GRID = 0x48;
const uint32_t DESCRIPTOR = 0x308;
const uint32_t KIND = 0x154;
const uint32_t RANGE = 0x198;
const uint32_t SHORT_RANGE_GRID = 1;
// The chain held 8 grids in testing. More means it isn't what we think.
const int MAX_GRIDS = 16;
// Streaming distance of the MainGrid (kind 0) and the LongRangeGrid (kind 2).
float g_streaming = 1.0f;

template <typename T> bool read(uintptr_t address, T &value)
{
    SIZE_T got = 0;
    return address != 0 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), &value, sizeof(T), &got) &&
           got == sizeof(T);
}

bool write_float(uintptr_t address, float value)
{
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), &value, sizeof(value), &done) &&
           done == sizeof(value);
}

uintptr_t world_of(uintptr_t object)
{
    uintptr_t scene = 0, handle = 0, world = 0;
    uint32_t flags = 0;
    if (!read(object + TO_SCENE, scene) || !read(scene + TO_HANDLE, handle) ||
        !read(handle + HANDLE_FLAGS, flags) || (flags & 0x80000000u) == 0 || !read(handle, world))
        return 0;
    return world;
}
} // namespace

bool grass_apply(uintptr_t lod_object, float multiplier)
{
    if (lod_object == 0 || !std::isfinite(multiplier) || multiplier < 1.0f || multiplier > 8.0f)
        return false;
    const uintptr_t world = world_of(lod_object);
    uintptr_t grid = 0;
    if (world == 0 || !read(world + GRIDS, grid))
        return false;
    // Collect first, write after: a chain that doesn't look right changes nothing.
    uintptr_t targets[MAX_GRIDS];
    float values[MAX_GRIDS];
    int count = 0;
    // The main and long-range grids follow while grass is raised. They go back to the game's range when grass does
    // (loads, the guard).
    // It follows gradually, 0.5x a second, so a switch never changes the loaded range in one step. Back at 1.0 grass
    // (a load) it is the game's range at once.
    static float stream_now = 1.0f;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::fmin(std::chrono::duration<float>(now - last).count(), 0.1f);
    last = now;
    const float stream_target = multiplier > 1.0f ? g_streaming : 1.0f;
    if (multiplier <= 1.0f)
        stream_now = 1.0f;
    else if (stream_now < stream_target)
        stream_now = std::fmin(stream_now + 0.5f * dt, stream_target);
    else
        stream_now = std::fmax(stream_now - 0.5f * dt, stream_target);
    const float stream = stream_now;
    for (int n = 0; grid != 0; ++n)
    {
        if (n == MAX_GRIDS)
            return false;
        uintptr_t descriptor = 0;
        uint32_t kind = 0;
        float range = 0.0f;
        if (!read(grid + DESCRIPTOR, descriptor) || !read(descriptor + KIND, kind) || kind > 3 ||
            !read(grid + RANGE, range) || !std::isfinite(range) || range <= 0.0f || range > 8.0f)
            return false;
        const float want = kind == SHORT_RANGE_GRID ? multiplier : kind == 0 || kind == 2 ? stream : range;
        if (want != range)
        {
            targets[count] = grid + RANGE;
            values[count++] = want;
        }
        if (!read(grid + NEXT_GRID, grid))
            return false;
    }
    for (int i = 0; i < count; ++i)
        write_float(targets[i], values[i]);
    return true;
}

void grass_set_streaming(float multiplier)
{
    if (std::isfinite(multiplier))
        g_streaming = std::fmin(std::fmax(multiplier, 1.0f), 2.0f);
}
