#pragma once

#include "ao.h"

// The scene lighting's use of the AO: baked shade blend, sunlight occlusion
// and bounce light. Their values live in the AO section. At the game's values
// the game's own shaders run.

void light_register();
void light_unregister();
// Takes the AO section's values and uses the lighting ones. Copies are made
// in the background the first time each variant is drawn after a change.
void light_set(const float values[AO_PARAMS]);

