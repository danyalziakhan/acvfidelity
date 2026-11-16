#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "lod_control.h"

#include <reshade.hpp>

#include "lod.h"
#include "lod_hook.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
// The game's own category for each of ours.
const int GAME_CATEGORY[LOD_CATEGORIES] = {0, 1, 2};

// A frame this long means a load, a fast travel or a time skip.
const double LONG_FRAME = 0.2;
// The camera moving faster than this is a jump, not travel. In meters a second. A galloping horse is about 15.
const float CAMERA_JUMP = 1000.0f;
// The game's values stay this long after the last sign of a load.
const double HOLD = 3.0;
// How fast detail rises, in distance multiplier a second. 1.0x to 2.5x takes 3 seconds.
const float RISE = 0.5f;

// Fields of the game's LOD object (see lod_hook.h).
const unsigned SECOND_DISTANCE = 0x74D4;
const unsigned CAMERA_SPEED = 0x74DC;

float g_target_distance = 1.0f;
float g_target[LOD_CATEGORIES] = {1.0f, 1.0f, 1.0f};
float g_distance = 1.0f; // applied now
float g_applied[LOD_CATEGORIES] = {1.0f, 1.0f, 1.0f};
float g_written = 1.0f; // the factor in the game's code
bool g_started = false;
bool g_hooked = false;
bool g_paused = true;
double g_hold_until = 0.0;
double g_last = 0.0;
// When the applied values started moving toward new targets, or 0 while they match them.
double g_rise_started = 0.0;

double now_seconds()
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0)
        QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return double(counter.QuadPart) / double(frequency.QuadPart);
}

// Toward a target. Down to the game's values at once, up at the rise rate.
float approach(float current, float target, float step)
{
    if (target <= current)
        return target;
    return std::min(target, current + step);
}

const char *load_sign(double frame, float second, float speed)
{
    if (frame > LONG_FRAME)
        return "a long frame";
    if (g_hooked && second == 0.0f)
        return "the game pausing its own detail";
    if (g_hooked && speed > CAMERA_JUMP)
        return "a camera jump";
    return nullptr;
}
} // namespace

void lod_control_set(float distance, const float categories[LOD_CATEGORIES])
{
    g_target_distance = distance;
    for (int k = 0; k < LOD_CATEGORIES; ++k)
        g_target[k] = categories[k];
}

void lod_control_frame()
{
    if (!lod_found() && !lod_find())
        return;
    const double now = now_seconds();
    if (!g_started)
    {
        g_started = true;
        g_hooked = lod_hook_install();
        dd_log(g_hooked ? reshade::log::level::info : reshade::log::level::warning,
               g_hooked ? "LOD: per-category distances active" : lod_hook_status());
        g_last = now;
        g_hold_until = now + HOLD; // the game is usually still loading
    }
    const double frame = now - g_last;
    g_last = now;

    float second = -1.0f, speed = 0.0f;
    if (g_hooked)
    {
        lod_hook_read(SECOND_DISTANCE, second);
        lod_hook_read(CAMERA_SPEED, speed);
    }
    if (const char *sign = load_sign(frame, second, speed))
    {
        g_hold_until = now + HOLD;
        if (!g_paused)
        {
            g_paused = true;
            char text[128];
            snprintf(text, sizeof(text), "LOD: %s, back to the game's own detail for now", sign);
            dd_log(reshade::log::level::info, text);
        }
    }
    else if (g_paused && now >= g_hold_until)
    {
        g_paused = false;
        dd_log(reshade::log::level::info, "LOD: loading over, detail rising back to the settings");
    }

    const float step = RISE * float(std::min(frame, 0.1));
    g_distance = approach(g_distance, g_paused ? 1.0f : g_target_distance, step);
    for (int k = 0; k < LOD_CATEGORIES; ++k)
        g_applied[k] = approach(g_applied[k], g_paused ? 1.0f : g_target[k], step);

    // One log line per climb: when the applied values reach the settings, and how long it took.
    bool reached = std::fabs(g_distance - g_target_distance) < 0.001f;
    for (int k = 0; k < LOD_CATEGORIES; ++k)
        reached &= std::fabs(g_applied[k] - g_target[k]) < 0.001f;
    if (g_paused || reached)
    {
        if (!g_paused && g_rise_started != 0.0)
        {
            char text[160];
            snprintf(text, sizeof(text),
                     "LOD: settings reached in %.1f s (detail %.2fx, objects %.2fx, clutter %.2fx, foliage %.2fx)",
                     now - g_rise_started, g_distance, g_applied[LOD_OBJECTS], g_applied[LOD_CLUTTER],
                     g_applied[LOD_FOLIAGE]);
            dd_log(reshade::log::level::info, text);
        }
        g_rise_started = 0.0;
    }
    else if (g_rise_started == 0.0)
        g_rise_started = now;

    const float factor = 1.0f / g_distance;
    if (std::fabs(factor - g_written) > 0.002f || (factor == 1.0f && g_written != 1.0f))
    {
        lod_write(factor);
        g_written = factor;
    }
    if (g_hooked)
    {
        for (int k = 0; k < LOD_CATEGORIES; ++k)
            lod_hook_set(GAME_CATEGORY[k], 1.0f / g_applied[k]);
    }
}


bool lod_control_paused()
{
    return g_paused;
}

bool lod_control_categories()
{
    return g_hooked;
}

void lod_control_shutdown()
{
    if (lod_found() && g_written != 1.0f)
        lod_write(1.0f);
    lod_hook_remove();
}
