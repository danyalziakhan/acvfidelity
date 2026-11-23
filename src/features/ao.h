#pragma once

#include "tuning.h"

#include <cstdint>

// Ambient occlusion. The game's AO is GTAO at half resolution with 2
// directions and 5 steps per side. A depth-aware blur follows in two passes.
// Every value below is a literal in copies of those shaders. A copy is bound
// whenever the game binds its own shader. Changes show while the game runs.
// At the game's values the game's own shaders run.
//
// AO only darkens ambient light. The game's lighting lifts it again in direct
// sun. In sunlit scenes strength shows little change above about 150%. In
// shade and indoors it keeps going. Sunlight occlusion lets it reach sunlight
// too (light.cpp).

enum ao_param
{
    AO_DIRECTIONS,
    AO_STEPS,
    AO_STRENGTH,
    AO_RADIUS,
    AO_FALLOFF_START,
    AO_FALLOFF_RANGE,
    AO_MIN_PIXELS,
    AO_MAX_PIXELS,
    AO_MAX_DISTANCE,
    AO_SPREAD,
    AO_MIN_OFFSET,
    AO_MIN_VISIBILITY,
    AO_THIN_COMPENSATION,
    AO_BLUR_WIDTH,
    AO_BLUR_DEPTH,
    AO_BLUR_NORMAL,
    // The scene lighting's use of the AO (light.cpp), from here on.
    AO_BOUNCE,
    AO_SUN,
    AO_SUN_POWER,
    AO_BLEND,
    AO_SKY,
    AO_CONTACT,
    AO_SKY_NIGHT,
    // The AO shader again: the method (0 horizon-based, the game's; 1 visibility bitmask) and the bitmask thickness.
    AO_METHOD,
    AO_THICKNESS,
    AO_PARAMS
};
const int AO_LIGHT_FIRST = AO_BOUNCE;
extern const param_def AO_DEFS[AO_PARAMS];
extern const float AO_PRESET_VALUES[PRESETS][AO_PARAMS];
// The window's order, in groups: look, light in shade, quality, shape of the shade. The settings not shown in the
// window follow.
const int AO_ORDER[AO_PARAMS] = {
    AO_STRENGTH,     AO_RADIUS,      AO_METHOD,        AO_THICKNESS,  AO_BOUNCE,      // look
    AO_SKY,          AO_SKY_NIGHT,   AO_SUN,           AO_BLEND,                      // light in shade
    AO_DIRECTIONS,   AO_STEPS,       AO_SPREAD,        AO_MAX_PIXELS, AO_MAX_DISTANCE, // quality
    AO_FALLOFF_START, AO_THIN_COMPENSATION, AO_MIN_OFFSET, AO_MIN_VISIBILITY, // shape
    AO_FALLOFF_RANGE, AO_MIN_PIXELS, AO_BLUR_WIDTH, AO_BLUR_DEPTH,
    AO_BLUR_NORMAL,  AO_SUN_POWER,   AO_CONTACT};
const int AO_DEFAULT_PRESET = 2; // Fidelity II

void ao_register();
void ao_unregister();
// Called at start and from the overlay. Makes the copies right away.
void ao_set(const float values[AO_PARAMS]);
// True once the game created its AO shader.
bool ao_ready();
// The window's text for a setting's value (the method's names), or nullptr for the plain number.
const char *ao_value_name(const param_def &def, float value);
// For a setting the method doesn't use at these values (Bitmask thickness with horizon-based): the window greys it
// out with this note. nullptr when it is in use.
const char *ao_inactive_note(int param, const float values[AO_PARAMS]);

