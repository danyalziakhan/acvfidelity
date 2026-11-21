#pragma once

// The time a model takes to blend into its next level of detail. The game keeps it in a component found through
// the camera (see lod_fade.cpp).

// Seconds. 0 leaves the game's value.
void lod_fade_set(float fade_seconds);
// Once a frame. Writes the value when it differs from what the game holds.
void lod_fade_frame();
// The game's own value as first read, or -1 before the component was found.
float lod_fade_game_fade();
