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
    AO_THIN,
    AO_FALLOFF_START,
    AO_FALLOFF_RANGE,
    AO_MIN_PIXELS,
    AO_MAX_PIXELS,
    AO_MAX_DISTANCE,
    AO_THIN_WIDTH,
    AO_THIN_TOLERANCE,
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
    AO_PARAMS
};
const int AO_LIGHT_FIRST = AO_BOUNCE;
extern const param_def AO_DEFS[AO_PARAMS];
extern const float AO_PRESET_VALUES[PRESETS][AO_PARAMS];
// The window's order, in groups: look, light in shade, quality, shape of the shade. The settings shown only in
// the debug build follow.
const int AO_ORDER[AO_PARAMS] = {
    AO_STRENGTH,     AO_RADIUS,      AO_BOUNCE,                                       // look
    AO_SKY,          AO_SKY_NIGHT,   AO_SUN,           AO_BLEND,                      // light in shade
    AO_DIRECTIONS,   AO_STEPS,       AO_SPREAD,        AO_MAX_PIXELS, AO_MAX_DISTANCE, // quality
    AO_FALLOFF_START, AO_THIN,       AO_THIN_COMPENSATION, AO_MIN_OFFSET, AO_MIN_VISIBILITY, // shape
    AO_FALLOFF_RANGE, AO_MIN_PIXELS, AO_THIN_WIDTH,    AO_THIN_TOLERANCE, AO_BLUR_WIDTH, AO_BLUR_DEPTH,
    AO_BLUR_NORMAL,  AO_SUN_POWER,   AO_CONTACT};
const int AO_DEFAULT_PRESET = 2; // Fidelity II

void ao_register();
void ao_unregister();
// Called at start and from the overlay. Makes the copies right away.
void ao_set(const float values[AO_PARAMS]);
// True once the game created its AO shader.
bool ao_ready();

