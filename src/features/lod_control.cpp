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
const double HOLD = 1.5;
// How fast detail rises, in distance multiplier a second. Clutter 1.0x to 4.0x takes 1.5 seconds.
const float RISE = 2.0f;
// Grass rises at the same rate, together with the others. 1.0x to 4.0x takes 1.5 seconds.
const float GRASS_RISE = 2.0f;

// Fields of the game's LOD object (see lod_hook.h).
const unsigned SECOND_DISTANCE = 0x74D4;
const unsigned CAMERA_SPEED = 0x74DC;
const unsigned CAMERA_HEIGHT = 0x88;
// The game's own draw distance for one class of objects. It grows while the game narrows the camera's view for a
// fast raven flight.
const unsigned MOTION_DISTANCE = 0x1864;

// The guard. Raised detail is safe at rest, but too much new detail at once can make the GPU fault: plants blink,
// geometry breaks and then the game crashes. That happens when the raven climbs fast or flies into a dense area.
// The guard watches how fast new objects are added, the camera's climb and the game's own motion distance. It eases
// every value toward Fidelity I while one of them is high, then brings them back slowly.
// New objects a second in the game's instance pool at which the guard counts as full. Fast flights at the game's
// own detail stayed under 8000. The GPU faults came after 25000 to 43000.
const double GROWTH_LIMIT = 15000.0;
// The camera's climb at which the guard counts as full, in meters a second. A raven boosting up climbs at 7 to 17.
const double CLIMB_LIMIT = 7.0;
// The game's motion distance above its resting value at which the guard counts as full. A raven boost reached 31%.
// Galloping and turning the camera reached 11%. So 10% eased detail while riding.
const double MOTION_LIMIT = 0.2;
// New objects count only while the camera is in the air. Galloping at Fidelity III grass added up to 23,000 a
// second. That is as many as the raven before a fault. Riding never corrupted. The camera counts as in the air for
// AIR_HOLD seconds after a climb of AIR_CLIMB over three seconds. A horse jump or downhill gallop measured 4.1 m/s.
// It also counts as in the air while it is AIR_HEIGHT meters above the lowest point of the last AIR_WINDOW seconds.
const double AIR_CLIMB = 6.0, AIR_HOLD = 30.0, AIR_HEIGHT = 50.0;
const int AIR_WINDOW = 120;
// Ease down at this share of a limit. Climb back after it stayed below the lower share for HOLD_LOW seconds.
const float GUARD_HIGH = 0.85f, GUARD_LOW = 0.75f;
const double HOLD_LOW = 10.0;
// Easing down, in distance multiplier a second.
const float GUARD_EASE = 1.0f;
// Climbing back: the other values over this many seconds, then grass over as many again.
const float GUARD_RESTORE = 25.0f;
const double GUARD_SAMPLE = 0.25;
const float NO_CEILING = 1000.0f;

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

// The guard's state. The ceilings are the most each value may be now: detail, the three categories, grass.
const int GUARDED = 2 + LOD_CATEGORIES;
float g_ceiling[GUARDED] = {NO_CEILING, NO_CEILING, NO_CEILING, NO_CEILING, NO_CEILING};
enum class guard_mode
{
    quiet,
    hold,
    easing,
    climbing,
    air,
};
// In the air the values hold at Fidelity I (detail, objects, clutter, foliage, grass) instead of climbing back
// step by step. The climb back at full raven height changed detail every few seconds: constant pop-in.
// Fidelity I never corrupted at the raven; II and III did.
const float AIR_CAP[GUARDED] = {1.5f, 1.67f, 2.0f, 1.67f, 2.0f};
// Seconds the values take from the settings down to Fidelity I when a raven boost starts: a fade, all values together.
// A boost to full height takes about 25 s; a corruption at Fidelity II set in about 9 s into the climb.
const float AIR_FADE = 6.0f;
guard_mode g_guard = guard_mode::quiet;
guard_mode g_guard_logged = guard_mode::quiet;
double g_guard_next = 0.0, g_guard_log = 0.0, g_low_since = -1.0;
float g_reads[4] = {};
int g_read_count = 0, g_read_at = 0;
float g_ratio = 0.0f;
// The last second of samples for growth and climb.
struct guard_sample
{
    double t;
    double instances; // 0 when the pool could not be read
    float height;
    bool height_ok;
};
// Four seconds of samples (every GUARD_SAMPLE).
const int SAMPLES = 16;
guard_sample g_samples[SAMPLES] = {};
int g_sample_count = 0, g_sample_at = 0;
double g_growth = 0.0, g_climb = 0.0, g_motion = 0.0;
float g_motion_rest = 0.0f;
// A camera cut: the height moved faster than any flight. A raven boost climbs at up to about 18 m/s. Going from the
// loading screen's scene into the world measured 208 m/s. Handled like a load, not as a climb.
const double CUT_SPEED = 50.0;
bool g_cut = false;
// In the air: until when, and the lowest camera height of each of the last AIR_WINDOW seconds.
double g_air_until = -1.0;
float g_low[AIR_WINDOW] = {};
int g_low_count = 0, g_low_at = 0;
double g_low_second = -1.0;
bool g_air = false;
// The new objects' share of their limit in the last read (0 on the ground). Only it takes the values below Fidelity I
// in the air: the climb of a raven boost eases to Fidelity I and no further.
double g_growth_ratio = 0.0;

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

template <typename T> bool read(uintptr_t address, T &value)
{
    SIZE_T got = 0;
    return address > 0x10000 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), &value, sizeof(T), &got) &&
           got == sizeof(T);
}

// Instances in use in the game's instance pool (Steam build 26142239). The path from the LOD object: world =
// [[[object + 0x7240] + 0x248]], scene = [world + 0x17a0], owner = [[scene + 0x5a8] + 0x10], pool at owner + 0x1350
// with its capacity at +0xe0 and free slots at +0xe8. The scene and its wrapper are checked by their vtables.
bool instances_in_use(uintptr_t object, double &used)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t s = 0, handle = 0, world = 0, scene = 0, wrapper = 0, owner = 0, vt = 0;
    uint32_t flags = 0, capacity = 0, free = 0;
    if (object == 0 || !read(object + 0x7240, s) || !read(s + 0x248, handle) || !read(handle + 8, flags) ||
        (flags & 0x80000000u) == 0 || !read(handle, world))
        return false;
    if (!read(world + 0x17a0, scene) || !read(scene, vt) || vt != base + 0x456ed58)
        return false;
    if (!read(scene + 0x5a8, wrapper) || !read(wrapper, vt) || vt != base + 0x4580820)
        return false;
    if (!read(wrapper + 0x10, owner) || owner == 0 || !read(owner + 0x1350 + 0xe0, capacity) ||
        !read(owner + 0x1350 + 0xe8, free) || free > capacity || capacity == free)
        return false;
    used = double(capacity - free);
    return true;
}

// One read of the three signals. The largest share of its limit counts. Negative when none could be read.
float guard_sample_now(double now)
{
    const uintptr_t object = lod_hook_object();
    guard_sample s = {now, 0.0, 0.0f, false};
    if (!instances_in_use(object, s.instances))
        s.instances = 0.0;
    s.height_ok = lod_hook_read(CAMERA_HEIGHT, s.height) && std::isfinite(s.height);
    g_samples[g_sample_at] = s;
    g_sample_at = (g_sample_at + 1) % SAMPLES;
    g_sample_count = std::min(g_sample_count + 1, SAMPLES);
    float r = -1.0f;
    double growth_r = -1.0;
    if (s.height_ok)
    {
        // The lowest height of each second, for "high above where the camera was".
        const double second = std::floor(now);
        if (second != g_low_second)
        {
            g_low_second = second;
            g_low_at = (g_low_at + 1) % AIR_WINDOW;
            g_low_count = std::min(g_low_count + 1, AIR_WINDOW);
            g_low[g_low_at] = s.height;
        }
        else
            g_low[g_low_at] = std::min(g_low[g_low_at], s.height);
    }
    // The newest sample at least `age` seconds old, or null.
    const auto older = [&](double age) -> const guard_sample * {
        for (int i = 1; i < g_sample_count; ++i)
        {
            const guard_sample &p = g_samples[(g_sample_at - 1 - i + 2 * SAMPLES) % SAMPLES];
            if (now - p.t >= age)
                return &p;
        }
        return nullptr;
    };
    // Growth and camera cuts over a second.
    if (const guard_sample *p = older(1.0))
    {
        if (s.instances > 0.0 && p->instances > 0.0)
        {
            g_growth = (s.instances - p->instances) / (now - p->t);
            growth_r = std::max(g_growth, 0.0) / GROWTH_LIMIT;
        }
        // Faster than this is a camera cut (a load, the raven called back), not a climb.
        if (s.height_ok && p->height_ok && std::fabs(s.height - p->height) / (now - p->t) >= CUT_SPEED)
            g_cut = true;
    }
    // The climb over three seconds. The camera swinging around Eivor while riding moved it up 8 m/s for a second and
    // back down. A raven boost keeps rising.
    if (const guard_sample *p = older(3.0); p != nullptr && s.height_ok && p->height_ok && !g_cut)
    {
        const double climb = (s.height - p->height) / (now - p->t);
        if (std::fabs(climb) < CUT_SPEED)
        {
            g_climb = climb;
            r = std::max(r, float(std::max(climb, 0.0) / CLIMB_LIMIT));
            if (climb >= AIR_CLIMB)
                g_air_until = now + AIR_HOLD;
        }
    }
    float motion = 0.0f;
    if (lod_hook_read(MOTION_DISTANCE, motion) && std::isfinite(motion) && motion > 0.0f && motion < 1e6f)
    {
        // The resting value is the lowest seen. It follows a lasting change slowly (over about 30 s), for example a
        // new field of view in the game's options.
        if (g_motion_rest <= 0.0f || motion < g_motion_rest)
            g_motion_rest = motion;
        else
            g_motion_rest += (motion - g_motion_rest) * float(GUARD_SAMPLE / 30.0);
        g_motion = motion / g_motion_rest - 1.0;
        // Logged, not a trigger: its resting value is learned, and the map opening, menus and camera changes moved
        // it 20-30% for half a minute with no streaming. The climb catches a raven boost.
    }
    bool high = false;
    if (s.height_ok && g_low_count > 0)
    {
        float lowest = g_low[g_low_at];
        for (int i = 1; i < g_low_count; ++i)
            lowest = std::min(lowest, g_low[(g_low_at - i + AIR_WINDOW) % AIR_WINDOW]);
        high = s.height - lowest >= AIR_HEIGHT;
    }
    g_air = high || now < g_air_until;
    g_growth_ratio = g_air ? std::max(growth_r, 0.0) : 0.0;
    if (g_air && growth_r >= 0.0)
        r = std::max(r, float(growth_r));
    return r;
}

const char *guard_name(guard_mode m)
{
    switch (m)
    {
    case guard_mode::quiet: return "quiet";
    case guard_mode::hold: return "holding";
    case guard_mode::easing: return "easing detail toward Fidelity I";
    case guard_mode::climbing: return "bringing detail back";
    case guard_mode::air: return "Fidelity I detail in the air";
    }
    return "?";
}

// Once a frame. The targets are the settings, the applied values what the game has now.
void guard_frame(double now, double frame, const float applied[GUARDED], const float target[GUARDED])
{
    if (!g_hooked)
        return;
    const float dt = float(std::min(frame, 0.1));
    if (now >= g_guard_next)
    {
        g_guard_next = now + GUARD_SAMPLE;
        const float v = guard_sample_now(now);
        if (v >= 0.0f)
        {
            g_reads[g_read_at] = v;
            g_read_at = (g_read_at + 1) % 4;
            g_read_count = std::min(g_read_count + 1, 4);
            float sorted[4];
            std::copy(g_reads, g_reads + g_read_count, sorted);
            std::sort(sorted, sorted + g_read_count);
            g_ratio = g_read_count % 2 == 1 ? sorted[g_read_count / 2]
                                            : 0.5f * (sorted[g_read_count / 2 - 1] + sorted[g_read_count / 2]);
            // A read at or above a limit counts at once.
            if (v >= 1.0f)
                g_ratio = std::max(g_ratio, v);
        }
    }
    bool limited = false;
    for (int k = 0; k < GUARDED; ++k)
        limited |= g_ceiling[k] < NO_CEILING;
    guard_mode next;
    if (g_read_count == 0)
        next = guard_mode::hold;
    else if (g_ratio >= GUARD_HIGH)
        next = guard_mode::easing;
    else if (g_ratio >= GUARD_LOW)
        next = guard_mode::hold;
    else
        next = g_guard == guard_mode::climbing ? guard_mode::climbing : guard_mode::quiet;
    if (next != guard_mode::quiet && next != guard_mode::climbing && next != guard_mode::air)
        g_low_since = -1.0;
    else if (g_low_since < 0.0)
        g_low_since = now;
    if (next == guard_mode::quiet && limited && now - g_low_since >= HOLD_LOW)
        next = guard_mode::climbing;
    if (next == guard_mode::hold && !limited)
        next = guard_mode::quiet;
    if (g_air && next != guard_mode::easing)
        next = guard_mode::air;
    else if (!g_air && next == guard_mode::air)
        next = guard_mode::quiet;
    g_guard = next;

    if (g_guard == guard_mode::easing)
    {
        // From what the game has now, down to Fidelity I (never below it). Too many new objects: fast. In the air
        // otherwise a fade over AIR_FADE seconds.
        const bool below = !g_air || g_growth_ratio >= GUARD_HIGH;
        for (int k = 0; k < GUARDED; ++k)
        {
            const float floor = std::min(AIR_CAP[k], std::max(target[k], 1.0f));
            const float now_k = std::min(g_ceiling[k], std::max(applied[k], 1.0f));
            // Too many new objects: fast. A boost: a fade over AIR_FADE seconds from the settings.
            const float rate = below ? GUARD_EASE : std::max(target[k] - floor, 0.05f) / AIR_FADE;
            g_ceiling[k] = now_k > floor ? std::max(floor, now_k - rate * dt) : now_k;
        }
    }
    else if (g_guard == guard_mode::air)
    {
        // Down to Fidelity I at the fade rate, never back up while in the air: climbing back at full raven height
        // streamed 15,000-25,500 new objects a second and eased again every ~8 s. Full detail returns
        // after landing.
        for (int k = 0; k < GUARDED; ++k)
        {
            const float cap = std::min(AIR_CAP[k], std::max(target[k], 1.0f));
            const float now_k = std::min(g_ceiling[k], std::max(applied[k], 1.0f));
            g_ceiling[k] = now_k > cap ? std::max(cap, now_k - std::max(target[k] - cap, 0.05f) / AIR_FADE * dt) : now_k;
        }
    }
    else if (g_guard == guard_mode::climbing)
    {
        // Detail and the categories first, grass last: grass adds the most new objects.
        bool others = false;
        for (int k = 0; k < GUARDED - 1; ++k)
            if (g_ceiling[k] < NO_CEILING)
            {
                others = true;
                g_ceiling[k] += std::max(target[k] - 1.0f, 0.05f) / GUARD_RESTORE * dt;
                if (g_ceiling[k] >= target[k])
                    g_ceiling[k] = NO_CEILING;
            }
        const int grass = GUARDED - 1;
        if (!others && g_ceiling[grass] < NO_CEILING)
        {
            g_ceiling[grass] += std::max(target[grass] - 1.0f, 0.05f) / GUARD_RESTORE * dt;
            if (g_ceiling[grass] >= target[grass])
                g_ceiling[grass] = NO_CEILING;
        }
        bool still = false;
        for (int k = 0; k < GUARDED; ++k)
            still |= g_ceiling[k] < NO_CEILING;
        if (!still)
            g_guard = guard_mode::quiet;
    }
    // One log line per change, and every 2 s while the values move.
    const bool moving = g_guard == guard_mode::easing || g_guard == guard_mode::climbing;
    if (g_guard != g_guard_logged || (moving && now >= g_guard_log))
    {
        char text[256];
        snprintf(text, sizeof(text),
                 "LOD guard: %s (new objects %.0f a second%s, climb %.1f m/s, motion %.0f%%); detail at most %.2fx, "
                 "grass at most %.2fx",
                 guard_name(g_guard), std::max(g_growth, 0.0), g_air ? ", in the air" : ", on the ground", g_climb,
                 g_motion * 100.0,
                 std::min(g_ceiling[0], target[0]), std::min(g_ceiling[GUARDED - 1], target[GUARDED - 1]));
        if (g_guard != g_guard_logged || g_guard != guard_mode::quiet)
            dd_log(reshade::log::level::info, text);
        g_guard_logged = g_guard;
        g_guard_log = now + 2.0;
    }
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
    if (g_cut)
    {
        g_cut = false;
        if (load == nullptr)
            load = "a camera cut";
    }
    if (const char *sign = load)
    {
        g_hold_until = now + HOLD;
        if (!g_paused)
        {
            g_paused = true;
            // A new scene: the guard starts over. Its ceilings, reads and the motion's resting value belong to the
            // old one. Detail rises from the game's own values the usual way after the pause.
            std::fill(g_ceiling, g_ceiling + GUARDED, NO_CEILING);
            g_guard = guard_mode::quiet;
            g_read_count = 0;
            g_sample_count = 0;
            g_motion_rest = 0.0f;
            g_low_since = -1.0;
            g_low_count = 0;
            g_air_until = -1.0;
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
        // The guard learns the new scene from here: its samples, the motion's resting value and the heights.
        g_read_count = 0;
        g_sample_count = 0;
        g_motion_rest = 0.0f;
        g_low_count = 0;
        g_air_until = -1.0;
        dd_log(reshade::log::level::info, "LOD: loading over, detail rising back to the settings");
    }
    if (game_paused != g_frozen)
    {
        g_frozen = game_paused;
        // Back from the map or a menu: the guard learns the scene again. A clamp from the map's opening camera goes.
        std::fill(g_ceiling, g_ceiling + GUARDED, NO_CEILING);
        g_guard = guard_mode::quiet;
        g_low_since = -1.0;
        g_read_count = 0;
        g_sample_count = 0;
        g_motion_rest = 0.0f;
        g_low_count = 0;
        g_air_until = -1.0;
        if (!g_paused)
            dd_log(reshade::log::level::info, g_frozen ? "LOD: the game paused its own detail (map or menu), "
                                                         "values held as they are"
                                                       : "LOD: the game's own detail resumed");
    }
    {
        const float applied[GUARDED] = {g_distance, g_applied[0], g_applied[1], g_applied[2], g_grass};
        const float target[GUARDED] = {g_target_distance, g_target[0], g_target[1], g_target[2], g_target_grass};
        // Not during a load, the map or a menu: their scenes have other resting values than the world.
        if (!g_paused && !g_frozen)
            guard_frame(now, frame, applied, target);
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
    // The settings, capped by the guard's ceilings. The ceilings move gradually, so the values follow gradually.
    const float target_distance = std::max(1.0f, std::min(g_target_distance, g_ceiling[0]));
    float target[LOD_CATEGORIES];
    for (int k = 0; k < LOD_CATEGORIES; ++k)
        target[k] = std::max(1.0f, std::min(g_target[k], g_ceiling[1 + k]));
    const float target_grass = std::max(1.0f, std::min(g_target_grass, g_ceiling[GUARDED - 1]));
    g_distance = approach(g_distance, low ? 1.0f : target_distance, step);
    for (int k = 0; k < LOD_CATEGORIES; ++k)
        g_applied[k] = approach(g_applied[k], low ? 1.0f : target[k], step);

    bool others = std::fabs(g_distance - target_distance) < 0.001f;
    for (int k = 0; k < LOD_CATEGORIES; ++k)
        others &= std::fabs(g_applied[k] - target[k]) < 0.001f;
    // Grass drops at once only for a load. Otherwise it eases both ways: a switch off never empties it in one step.
    if (low)
        g_grass = 1.0f;
    else
        g_grass = approach(g_grass, target_grass, GRASS_RISE * float(std::min(frame, 0.1)));

    // One log line per climb: when the applied values reach the settings and how long it took.
    const bool reached = others && std::fabs(g_grass - target_grass) < 0.001f;
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

bool lod_control_guarded()
{
    for (int k = 0; k < GUARDED; ++k)
        if (g_ceiling[k] < NO_CEILING)
            return true;
    return false;
}

void lod_control_shutdown()
{
    if (lod_found() && g_written != 1.0f)
        lod_write(1.0f);
    lod_hook_remove();
}
