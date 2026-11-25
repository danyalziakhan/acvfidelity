#pragma once

// What the game has set on each command list, followed from the start: the root signature, pipeline and root
// arguments of both bind points, and the render targets. The scene hook (scene_inject.h) reads it at the lighting
// draw and puts the game's state back after its own passes. Also the buffer behind a root descriptor's GPU address
// and the shader hash of each compute pipeline.

#include <reshade.hpp>

#include <cstdint>

namespace scene_state
{
using namespace reshade::api;

const uint32_t MAX_PARAMS = 32;    // root parameters followed per root signature
const uint32_t MAX_CONSTANTS = 64; // a D3D12 root signature holds at most 64 DWORDs
const uint32_t MAX_TARGETS = 8;

enum class root_kind : uint8_t
{
    none,
    table,     // value.table
    buffer,    // a root CBV, SRV or UAV: value.buffer, of type value.type
    constants, // root constants: value.constants[0 .. constant_count)
};

struct root_value
{
    root_kind kind;
    descriptor_type type;
    uint64_t table;      // descriptor_table handle
    buffer_range buffer; // root descriptor
    uint32_t constant_count;
    uint32_t constants[MAX_CONSTANTS];
};

// One pipeline bind point (graphics or compute) of a command list, as the game has set it up to the current draw
// or dispatch.
struct root_state
{
    uint64_t layout;   // pipeline_layout handle bound for this bind point
    uint64_t pipeline; // pipeline handle bound for this bind point
    uint32_t param_count;
    root_value params[MAX_PARAMS];
};

struct targets
{
    uint32_t count;
    resource_view rtv[MAX_TARGETS];
    resource_view dsv; // 0 when no depth-stencil is bound
};

// At add-on load, before the scene hook's events, so nothing the game creates early is missed.
void register_events();
void unregister_events();
// The game's root arguments and render targets on a command list as followed so far (the add-on's own native calls
// are not seen), or nullptr. Valid while that list is being recorded on this thread.
const root_state *root_of(command_list *cmd, bool compute);
const targets *targets_of(command_list *cmd);
// The FNV-1a 64 hash of a compute pipeline's shader. False for graphics pipelines and unknown ones.
bool compute_hash(uint64_t pipeline, uint64_t &hash);
} // namespace scene_state
