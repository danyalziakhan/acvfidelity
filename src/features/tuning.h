#pragma once

// A setting carried as a literal in a copy of one of the game's shaders. The
// AO and reflection modules each list theirs. Everything else works from
// these lists.
struct param_def
{
    const char *key;     // in its section of acvfidelity.ini
    const char *label;   // in the window
    const char *tooltip; // nullptr for none
    float game;          // the game's own value
    float min, max;
    const char *format;  // ImGui slider format
    bool integer;
    bool logarithmic;
    bool advanced;       // not shown in the window
};

// The quality presets every section offers. "Custom" is shown when the
// values match none of them.
const int PRESETS = 4;
const char *const PRESET_NAMES[PRESETS + 1] = {"Game", "Fidelity I", "Fidelity II", "Fidelity III", "Custom"};
