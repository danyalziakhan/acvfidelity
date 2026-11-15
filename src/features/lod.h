#pragma once

#include <string>

// The game's LOD factor. Every frame the game writes 1.0 into a field of its
// render setup. The field scales the distance at which objects switch to less
// detailed models. Lower keeps the detailed ones farther away. The 1.0 is an
// immediate in the instruction itself. Changing it in the running game's code
// changes the factor from the next frame on.

// Looks for the instruction in the game's code. True once found.
bool lod_find();
bool lod_found();
// Where it was found or why it wasn't. Shown in the window and the log.
const std::string &lod_where();
// Writes a new factor into the instruction.
void lod_write(float factor);
// The factor the instruction holds now.
float lod_current();
