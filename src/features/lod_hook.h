#pragma once

#include <cstdint>

// The game splits its LOD factor into five per-category scales. A detour where it stores them records the LOD
// object's address every frame. It multiplies each scale by a value set live. The detour is found by pattern
// and sits at ACValhalla.exe+0x935220 in Steam build 26142239. The scaled values go to both copies the game
// keeps. One is read by streaming and one by drawing, so the two always agree on the model. Lower keeps that
// category's detailed models farther away.

bool lod_hook_install();
void lod_hook_remove();
bool lod_hook_active();
void lod_hook_set(int category, float multiplier);
float lod_hook_get(int category);
// The LOD object from the last frame, or 0.
uintptr_t lod_hook_object();
// Reads a float of the LOD object safely. False if it can't be read.
bool lod_hook_read(uint32_t offset, float &value);
const char *lod_hook_status();
