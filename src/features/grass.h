#pragma once

// Grass distance. Ground clutter such as grass, flowers and small plants is stored in the world data. It only
// exists in the world cells the game has loaded. The ShortRangeGrid streams those cells. A multiplier on each
// such grid scales how far out it loads them. The game keeps two of these grids, one per world layer. Both get the same
// multiplier. 1.0 is the game's own. The grids are rebuilt on loads. So the value is written again every
// frame. Only the grids' own multiplier changes. The game's per-collection streaming limit stays as it is.
// Lifting it streamed so much grass at once that the GPU faulted in testing.

// The multiplier for every ShortRangeGrid. Called once a frame with the LOD object the LOD hook records.
// Returns false if the grids couldn't be found or didn't look as expected. Nothing is written then.
bool grass_apply(uintptr_t lod_object, float multiplier);
