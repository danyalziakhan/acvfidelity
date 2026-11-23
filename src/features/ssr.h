#pragma once

#include "tuning.h"

// Screen-space reflections. The game's SSR is a half-resolution full-screen
// pass for water and flat surfaces. Every value below is a literal in a copy
// of its pixel shader. The copy is bound whenever the game binds its own
// pipeline. Changes show while the game runs. At the game's values the
// game's own pipeline runs. Higher always means more of what the name says.

enum ssr_param
{
    SSR_SMOOTHING,
    SSR_THICKNESS,
    SSR_LENGTH,
    SSR_STEPS,
    SSR_DISTANCE_SMOOTHING,
    SSR_FAR_DETAIL,
    SSR_RAY_CUTOFF,
    SSR_SHORT_THICKNESS,
    SSR_FAR_THICKNESS,
    SSR_JITTER,
    SSR_JITTER_RAMP,
    SSR_START_OFFSET,
    SSR_EDGE_Y,
    SSR_EDGE_X,
    SSR_EDGE_X_SHARPNESS,
    SSR_EDGE_Y_SHARPNESS,
    SSR_SKY,
    SSR_INTENSITY,
    SSR_NOISE,
    SSR_SOFTEN,
    SSR_REFINE,
    SSR_UPSCALE,
    SSR_UPSCALE_FILTER,
    SSR_ANGLE,
    SSR_RESOLUTION,
    SSR_MIP_SCALE,
    SSR_HISTORY_MOTION,
    SSR_PARAMS
};
extern const param_def SSR_DEFS[SSR_PARAMS];
// The window's order, in groups: look (where reflections show), tracing, image. The settings not shown in the
// window follow.
const int SSR_ORDER[SSR_PARAMS] = {
    SSR_SMOOTHING,     SSR_EDGE_Y,         SSR_ANGLE,                                         // look
    SSR_STEPS,         SSR_REFINE,         SSR_THICKNESS,     SSR_FAR_THICKNESS, SSR_JITTER, SSR_NOISE, // tracing
    SSR_RESOLUTION,    SSR_HISTORY_MOTION, SSR_UPSCALE,       SSR_SOFTEN,                         // image
    SSR_UPSCALE_FILTER, SSR_LENGTH,        SSR_DISTANCE_SMOOTHING, SSR_FAR_DETAIL, SSR_RAY_CUTOFF,
    SSR_SHORT_THICKNESS, SSR_JITTER_RAMP,  SSR_START_OFFSET,  SSR_EDGE_X,        SSR_EDGE_X_SHARPNESS,
    SSR_EDGE_Y_SHARPNESS, SSR_SKY,         SSR_INTENSITY,     SSR_MIP_SCALE};
extern const float SSR_PRESET_VALUES[PRESETS][SSR_PARAMS];
const int SSR_DEFAULT_PRESET = 2; // Fidelity II

void ssr_register();
void ssr_unregister();
// Called at start and from the overlay. Makes the copy right away.
void ssr_set(const float values[SSR_PARAMS]);
// True once the game created its SSR pipeline.
bool ssr_ready();
// The window's text for a setting's value (Half / Three quarters / Full, Off / On), or nullptr for the plain number.
const char *ssr_value_name(const param_def &def, float value);
// Why a setting is greyed out, or nullptr when it applies.
const char *ssr_inactive_note(int param, const float values[SSR_PARAMS]);
