#pragma once

// Level of detail as the player sets it: the overall detail distance and three per-category distances.
// Every frame the values applied to the game move toward these targets. They rise gradually and drop back
// to the game's own values at once during loads, fast travel and camera jumps. The game streams in many
// models at those moments. Too much detail on top of that can hang the GPU.

enum lod_category
{
    LOD_OBJECTS, // the game's category 0: rocks, props, bushes, some trees
    LOD_CLUTTER, // category 1: small plants and clutter near the player
    LOD_FOLIAGE, // category 2: leaves and branches of trees and large plants
    LOD_CATEGORIES
};

// Distances as multipliers of the game's own (1.0). Higher keeps detailed models farther away.
void lod_control_set(float distance, const float categories[LOD_CATEGORIES]);
// Once a frame from the present event. Installs the category hook on its first call.
void lod_control_frame();
// True while loading or right after it, when the game's own values are in use.
bool lod_control_paused();
// False if the categories couldn't be hooked. The overall distance still works then.
bool lod_control_categories();
// Gives the game its own values back. From DLL_PROCESS_DETACH.
void lod_control_shutdown();
