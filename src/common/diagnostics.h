#pragma once

#include <reshade.hpp>

// Detailed logging, Debug=1 under [Diagnostics] in acvfidelity.ini (see diagnostics.cpp).

enum diagnostics_pass
{
    PASS_AO,
    PASS_AO_BLUR_H,
    PASS_AO_BLUR_V,
    PASS_REFLECTIONS,
    PASSES
};

void diagnostics_start(bool on);
void diagnostics_stop();
bool diagnostics_on();
// From the bind hooks: the game bound one of the passes we change, as our copy or its own.
void diagnostics_bound(reshade::api::command_list *cmd, int pass, bool ours);
// Once a frame.
void diagnostics_present(reshade::api::command_queue *queue);
