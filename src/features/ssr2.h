#pragma once

// Our own screen-space reflections for every glossy surface, in place of the probe reflection of the scene
// lighting. Runs on the scene hook (scene_inject.h). In the window "Screen-space reflections (Surfaces)", ini
// [ScreenSpaceReflectionsSurfaces]. On while Strength > 0.

#include "tuning.h"

enum material_param
{
    MAT_STRENGTH,
    MAT_MAX_ROUGH,
    MAT_STEPS,
    MAT_SCALE,
    MAT_FADE,
    MAT_THICKNESS,
    MAT_TRIM,
    MAT_RADIUS,
    MAT_CAP,
    MAT_BLEND,
    MAT_LENGTH,
    MAT_MOVE_LOW,
    MAT_MOVE_HIGH,
    MAT_CHARACTERS,
    MAT_CHARACTER_HITS,
    MAT_PARAMS
};
extern const param_def MAT_DEFS[MAT_PARAMS];
extern const float MAT_PRESET_VALUES[PRESETS][MAT_PARAMS];
const int MAT_DEFAULT_PRESET = 2; // Fidelity II, like every section

// Applies the values: on while Strength > 0 and the add-on is on. Before ssr2_start only stores them.
void ssr2_set(const float values[MAT_PARAMS]);
// Once the game has drawn a while: the values from then on take effect.
void ssr2_start();
void ssr2_register(); // at load, after scene::register_events
