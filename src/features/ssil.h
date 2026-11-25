#pragma once

// Screen-space indirect light: one diffuse bounce from a visibility bitmask. Runs on the scene hook
// (scene_inject.h). Our visibility takes the place of the game's screen AO in its own occlusion of the ambient, and
// our bounce is added. In the window "Indirect lighting", ini [IndirectLighting]. On while Strength > 0.

#include "tuning.h"

enum ssil_param
{
    SSIL_STRENGTH,
    SSIL_RADIUS,
    SSIL_STEPS,
    SSIL_SLICES,
    SSIL_SCALE,
    SSIL_HISTORY,
    SSIL_CAP,
    SSIL_BLEND,
    SSIL_FALLOFF,
    SSIL_SPREAD,
    SSIL_FACING,
    SSIL_PASSES,
    SSIL_MAX_PIXELS,
    SSIL_DEPTH_TOLERANCE,
    SSIL_NORMAL_POWER,
    SSIL_PARAMS
};
extern const param_def SSIL_DEFS[SSIL_PARAMS];
extern const float SSIL_PRESET_VALUES[PRESETS][SSIL_PARAMS];
const int SSIL_DEFAULT_PRESET = 2; // Fidelity II, like every section

// Applies the values: on while Strength > 0 and the add-on is on. Thickness is the AO section's bitmask thickness
// in meters. Before ssil_start only stores them.
// The thickness and the radius come from the ambient occlusion section.
// The window's order: Strength, then Directions before Steps as in the ambient occlusion section. The rest is hidden.
const int SSIL_ORDER[SSIL_PARAMS] = {SSIL_STRENGTH, SSIL_RADIUS, SSIL_SLICES, SSIL_STEPS, SSIL_SCALE, SSIL_HISTORY,
                                     SSIL_CAP, SSIL_BLEND, SSIL_FALLOFF, SSIL_SPREAD, SSIL_FACING, SSIL_PASSES,
                                     SSIL_MAX_PIXELS, SSIL_DEPTH_TOLERANCE, SSIL_NORMAL_POWER};
void ssil_set(const float values[SSIL_PARAMS], float thickness);
// Once the game has drawn a while: the values from then on take effect.
void ssil_start();
void ssil_register(); // at load, after scene::register_events
