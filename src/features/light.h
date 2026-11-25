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

// Surface reflections: 0 off, else the space-2 register the second lighting copy reads our reflection texture from.
// With gloss the copy also stores its final gloss into our UAV at space-2 u0 (only once the root signature is known
// to have it).
void light_set_ssr2(uint32_t reg, bool gloss);
// Only the gloss flag (the scene hook sets it once it has seen u0 in the lighting's table). Changes nothing while the
// reflection texture is off.
void light_set_ssr2_gloss(bool gloss);
// Indirect lighting: 0 off, else the space-2 register the scene copy reads our indirect light texture from. One copy
// per variant carries whichever of the two is on (light_ssr_pipelines gives it).
void light_set_ssil(uint32_t reg);
// For a game lighting pipeline: its scene copy (0 while none exists) and what to bind without ours (the AO copy or
// the game's own), its hash and why it has no scene copy for good (nullptr while one can still be made). False for
// any other pipeline.
bool light_ssr_pipelines(uint64_t pipe, uint64_t *ssr, uint64_t *fallback, uint64_t *hash, const char **missing);
