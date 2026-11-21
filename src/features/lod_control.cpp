#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "lod_control.h"

#include <reshade.hpp>

#include "lod.h"
#include "grass.h"
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
// How fast detail rises, in distance multiplier a second. 1.0x to 2.5x takes 2.4 seconds.
const float RISE = 0.625f;
// Grass rises slower. 1.0x to 3.0x takes 6.4 seconds. Its extra cells then stream in gradually. It also waits until
// the other values have reached their settings. After a load the two streaming loads then come one after the
// other.
const float GRASS_RISE = 0.3125f;

// Fields of the game's LOD object (see lod_hook.h).
const unsigned SECOND_DISTANCE = 0x74D4;
const unsigned CAMERA_SPEED = 0x74DC;

float g_target_distance = 1.0f;
float g_target[LOD_CATEGORIES] = {1.0f, 1.0f, 1.0f};
float g_distance = 1.0f; // applied now
float g_applied[LOD_CATEGORIES] = {1.0f, 1.0f, 1.0f};
float g_written = 1.0f; // the factor in the game's code
float g_target_grass = 1.0f;
float g_grass = 1.0f;     // applied now
bool g_grass_found = true; // last result, for one log line per change
bool g_started = false;
bool g_hooked = false;
bool g_paused = true;
bool g_frozen = false; // the game paused its own detail outside a load: values held
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

const char *load_sign(double frame, float speed)
{
    if (frame > LONG_FRAME)
        return "a long frame";
    if (g_hooked && speed > CAMERA_JUMP)
        return "a camera jump";
    return nullptr;
}
} // namespace

void lod_control_set(float distance, const float categories[LOD_CATEGORIES], float grass)
{
    g_target_distance = distance;
    g_target_grass = grass;
    for (int k = 0; k < LOD_CATEGORIES; ++k)
        g_target[k] = categories[k];
}

namespace
{
void update_values(double now, double frame);
void write_values();
} // namespace

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
    // The game zeroes its second distance in the map and menus, and during loads. In the map and menus the
    // values stay where they are. Dropping them there made detail rise again on return. That showed as pop-in.
    // Inside a load it keeps the game's own values.
    const bool game_paused = g_hooked && second == 0.0f;
    const char *load = load_sign(frame, speed);
    if (const char *sign = load)
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
    else if (g_paused && game_paused)
        g_hold_until = now + HOLD;
    else if (g_paused && now >= g_hold_until)
    {
        g_paused = false;
        dd_log(reshade::log::level::info, "LOD: loading over, detail rising back to the settings");
    }
    if (game_paused != g_frozen)
    {
        g_frozen = game_paused;
        if (!g_paused)
            dd_log(reshade::log::level::info, g_frozen ? "LOD: the game paused its own detail (map or menu), "
                                                         "values held as they are"
                                                       : "LOD: the game's own detail resumed");
    }
    // Held as they are while the game pauses its detail outside a load. They are still written below.
    bool update = !g_frozen || g_paused;
    if (update)
        update_values(now, frame);
    write_values();
}

namespace
{
void update_values(double now, double frame)
{
    const bool low = g_paused;
    const float step = RISE * float(std::min(frame, 0.1));
    g_distance = approach(g_distance, low ? 1.0f : g_target_distance, step);
    for (int k = 0; k < LOD_CATEGORIES; ++k)
        g_applied[k] = approach(g_applied[k], low ? 1.0f : g_target[k], step);

    bool others = std::fabs(g_distance - g_target_distance) < 0.001f;
    for (int k = 0; k < LOD_CATEGORIES; ++k)
        others &= std::fabs(g_applied[k] - g_target[k]) < 0.001f;
    // Grass drops at once and climbs only after the rest.
    if (low || g_target_grass <= g_grass)
        g_grass = low ? 1.0f : g_target_grass;
    else if (others)
        g_grass = approach(g_grass, g_target_grass, GRASS_RISE * float(std::min(frame, 0.1)));

    // One log line per climb: when the applied values reach the settings and how long it took.
    const bool reached = others && std::fabs(g_grass - g_target_grass) < 0.001f;
    if (low || reached)
    {
        if (!low && g_rise_started != 0.0)
        {
            char text[160];
            snprintf(text, sizeof(text),
                     "LOD: settings reached in %.1f s (detail %.2fx, objects %.2fx, clutter %.2fx, foliage %.2fx, "
                     "grass %.2fx)",
                     now - g_rise_started, g_distance, g_applied[LOD_OBJECTS], g_applied[LOD_CLUTTER],
                     g_applied[LOD_FOLIAGE], g_grass);
            dd_log(reshade::log::level::info, text);
        }
        g_rise_started = 0.0;
    }
    else if (g_rise_started == 0.0)
        g_rise_started = now;
}

void write_values()
{
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
        const bool found = grass_apply(lod_hook_object(), g_grass);
        if (found != g_grass_found)
        {
            g_grass_found = found;
            dd_log(found ? reshade::log::level::info : reshade::log::level::warning,
                   found ? "Grass distance: streaming grids found"
                         : "Grass distance: the streaming grids were not found or looked different. The game's own "
                           "range stays");
        }
    }
}
} // namespace


bool lod_control_paused()
{
    return g_paused;
}


bool lod_control_categories()
{
    return g_hooked;
}

bool lod_control_grass()
{
    return g_grass_found;
}

void lod_control_shutdown()
{
    if (lod_found() && g_written != 1.0f)
        lod_write(1.0f);
    lod_hook_remove();
}
