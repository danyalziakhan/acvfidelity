#pragma once

#include <cstdint>

// How far the camera can look down before the game fades its screen-space reflections out. The game fades them
// from about 25 to 35 degrees below level and then skips the pass. The thresholds live in the water settings
// object (see ssr_angle.cpp).

// The angle in degrees where the fade ends. 35 or less leaves the game's own values.
void ssr_angle_set(float degrees);
// Every frame, with the LOD object (lod_hook_object) for the path to the world.
void ssr_angle_frame(uintptr_t lod_object);
// Water reflections resolution: 0 the game's half, 1 three quarters, 2 full. Written into the river's resolution
// object.
void ssr_res_set(int mode);
// Puts the game's half resolution back if we changed it (unload).
void ssr_res_restore();
// The live SSR target against the render (checked every 30 frames): 0 = half, 1 = three quarters, 2 = full, -1 other or
// not known.
int ssr_res_live();
