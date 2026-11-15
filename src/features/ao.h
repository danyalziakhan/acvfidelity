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
// shade and indoors it keeps going.

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
    AO_BLUR_WIDTH,
    AO_BLUR_DEPTH,
    AO_BLUR_NORMAL,
    AO_PARAMS
};
extern const param_def AO_DEFS[AO_PARAMS];
extern const float AO_PRESET_VALUES[PRESETS][AO_PARAMS];
const int AO_DEFAULT_PRESET = 2; // Fidelity II

void ao_register();
void ao_unregister();
// Called at start and from the overlay. Makes the copies right away.
void ao_set(const float values[AO_PARAMS]);
// True once the game created its AO shader.
bool ao_ready();

