// Root signatures and pipelines are created once at the start. Root arguments are set long before the draw that uses
// them. So all of that is followed here all the time, and the scene hook only asks.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include "scene_state.h"
#include "descriptors.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

using namespace reshade::api;

namespace scene_state
{
namespace
{
const uint64_t FNV_BASIS = 1469598103934665603ull;
const uint64_t FNV_PRIME = 1099511628211ull;

uint64_t fnv(uint64_t hash, const void *data, size_t size)
{
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * FNV_PRIME;
    return hash;
}

// Per pipeline: compute or graphics, and the compute shader's hash.
struct pipeline_info
{
    bool compute;
    uint64_t hash;
};
std::shared_mutex g_pipelines_lock;
std::unordered_map<uint64_t, pipeline_info> g_pipelines;

// Root SRVs and UAVs come with a GPU address only. So buffers are kept by address to name the buffer behind one, as
// ReShade does for root CBVs. Placed buffers can share addresses. The newest one wins.
struct gpu_buffer
{
    uint64_t handle;
    uint64_t size;
};
std::shared_mutex g_addresses_lock;
std::map<uint64_t, gpu_buffer> g_by_address;
std::unordered_map<uint64_t, uint64_t> g_address_of;

void on_init_pipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline pipe)
{
    pipeline_info info = {false, FNV_BASIS};
    for (uint32_t i = 0; i < count; ++i)
        if (subobjects[i].type == pipeline_subobject_type::compute_shader && subobjects[i].data != nullptr &&
            subobjects[i].count != 0)
        {
            const auto *shader = static_cast<const shader_desc *>(subobjects[i].data);
            info.compute = true;
            if (shader->code != nullptr)
                info.hash = fnv(info.hash, shader->code, shader->code_size);
        }
    std::unique_lock lock(g_pipelines_lock);
    g_pipelines[pipe.handle] = info;
}

void on_destroy_pipeline(device *, pipeline pipe)
{
    std::unique_lock lock(g_pipelines_lock);
    g_pipelines.erase(pipe.handle);
}

void on_init_resource(device *dev, const resource_desc &desc, const subresource_data *, resource_usage, resource res)
{
    if (res.handle == 0 || dev->get_api() != device_api::d3d12 || desc.type != resource_type::buffer)
        return;
    const uint64_t address = reinterpret_cast<ID3D12Resource *>(res.handle)->GetGPUVirtualAddress();
    if (address == 0)
        return;
    std::unique_lock lock(g_addresses_lock);
    g_by_address[address] = {res.handle, desc.buffer.size};
    g_address_of[res.handle] = address;
}

void on_destroy_resource(device *, resource res)
{
    std::unique_lock lock(g_addresses_lock);
    const auto it = g_address_of.find(res.handle);
    if (it == g_address_of.end())
        return;
    const auto at = g_by_address.find(it->second);
    if (at != g_by_address.end() && at->second.handle == res.handle)
        g_by_address.erase(at);
    g_address_of.erase(it);
}

// The buffer and offset at a GPU address, or the address itself as the offset of no buffer when it is not known.
buffer_range resolve(uint64_t address)
{
    std::shared_lock lock(g_addresses_lock);
    auto it = g_by_address.upper_bound(address);
    if (it != g_by_address.begin())
    {
        --it;
        if (address - it->first < it->second.size)
            return {{it->second.handle}, address - it->first, UINT64_MAX};
    }
    return {{0}, address, 0};
}

struct __declspec(uuid("5b93d0e4-27c1-4af8-9e6d-0c3a8f71b2d5")) list_state
{
    root_state roots[2]; // graphics, compute
    targets bound;
};

list_state &state_of(command_list *cmd)
{
    if (list_state *s = cmd->get_private_data<list_state>())
        return *s;
    return *cmd->create_private_data<list_state>();
}

bool has_compute(shader_stage stages)
{
    return (stages & shader_stage::all_compute) != 0;
}

bool has_graphics(shader_stage stages)
{
    return (stages & shader_stage::all_graphics) != 0;
}

void clear(root_state &root)
{
    root.layout = 0;
    root.pipeline = 0;
    root.param_count = 0;
}

// The value of a root parameter, after the arguments of a different root signature were dropped (D3D12 drops them
// too). Parameters past the ones set so far are cleared as they come into use. So a reset only has to set the count
// back.
root_value *param_of(root_state &root, pipeline_layout layout, uint32_t index)
{
    if (root.layout != layout.handle)
    {
        root.layout = layout.handle;
        root.param_count = 0;
    }
    if (index >= MAX_PARAMS)
        return nullptr;
    for (; root.param_count <= index; ++root.param_count)
        root.params[root.param_count] = {};
    return &root.params[index];
}

void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipe)
{
    list_state &s = state_of(cmd);
    if (pipe.handle == 0)
    {
        s.roots[0].pipeline = 0;
        s.roots[1].pipeline = 0;
        return;
    }
    // D3D12 binds every pipeline with pipeline_stage::all, so which bind point it belongs to comes from how it was
    // created.
    int point = -1;
    {
        std::shared_lock lock(g_pipelines_lock);
        const auto it = g_pipelines.find(pipe.handle);
        if (it != g_pipelines.end())
            point = it->second.compute ? 1 : 0;
    }
    if (point < 0)
    {
        const bool compute = (stages & pipeline_stage::all_compute) != 0;
        const bool graphics = (stages & pipeline_stage::all_graphics) != 0;
        if (compute == graphics)
            return;
        point = compute ? 1 : 0;
    }
    s.roots[point].pipeline = pipe.handle;
}

void on_bind_tables(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
                    const descriptor_table *tables, uint32_t, const uint32_t *)
{
    list_state &s = state_of(cmd);
    for (int point = 0; point < 2; ++point)
    {
        if (!(point == 1 ? has_compute(stages) : has_graphics(stages)))
            continue;
        root_state &root = s.roots[point];
        // A root signature change comes as an empty bind.
        if (root.layout != layout.handle)
        {
            root.layout = layout.handle;
            root.param_count = 0;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            root_value *v = param_of(root, layout, first + i);
            if (v == nullptr)
                break;
            v->kind = root_kind::table;
            v->table = tables[i].handle;
        }
    }
}

void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t param,
                         const descriptor_table_update &update)
{
    if (update.count == 0 || update.descriptors == nullptr)
        return;
    buffer_range range;
    switch (update.type)
    {
    case descriptor_type::constant_buffer:
        range = *static_cast<const buffer_range *>(update.descriptors);
        break;
    // Root SRVs and UAVs carry the GPU address, not a buffer range.
    case descriptor_type::buffer_shader_resource_view:
    case descriptor_type::buffer_unordered_access_view:
    case descriptor_type::acceleration_structure:
        range = resolve(*static_cast<const uint64_t *>(update.descriptors));
        break;
    default:
        return;
    }
    list_state &s = state_of(cmd);
    for (int point = 0; point < 2; ++point)
    {
        if (!(point == 1 ? has_compute(stages) : has_graphics(stages)))
            continue;
        root_value *v = param_of(s.roots[point], layout, param);
        if (v == nullptr)
            continue;
        v->kind = root_kind::buffer;
        v->type = update.type;
        v->buffer = range;
    }
}

void on_push_constants(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first,
                       uint32_t count, const void *values)
{
    if (first >= MAX_CONSTANTS || values == nullptr)
        return;
    count = std::min(count, MAX_CONSTANTS - first);
    list_state &s = state_of(cmd);
    for (int point = 0; point < 2; ++point)
    {
        if (!(point == 1 ? has_compute(stages) : has_graphics(stages)))
            continue;
        root_value *v = param_of(s.roots[point], layout, param);
        if (v == nullptr)
            continue;
        v->kind = root_kind::constants;
        memcpy(v->constants + first, values, count * sizeof(uint32_t));
        v->constant_count = std::max(v->constant_count, first + count);
    }
}

void on_bind_targets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
    targets &t = state_of(cmd).bound;
    t.count = std::min(count, MAX_TARGETS);
    for (uint32_t i = 0; i < t.count; ++i)
        t.rtv[i] = rtvs != nullptr ? rtvs[i] : resource_view{0};
    t.dsv = dsv;
}

void on_reset(command_list *cmd)
{
    if (list_state *s = cmd->get_private_data<list_state>())
    {
        clear(s->roots[0]);
        clear(s->roots[1]);
        s->bound.count = 0;
        s->bound.dsv = {0};
    }
}

void on_destroy_list(command_list *cmd)
{
    cmd->destroy_private_data<list_state>();
}
} // namespace

void register_events()
{
    descriptor_tracker::register_events();
    reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
    reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_bind_tables);
    reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
    reshade::register_event<reshade::addon_event::push_constants>(on_push_constants);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_targets);
    reshade::register_event<reshade::addon_event::reset_command_list>(on_reset);
    reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_list);
}

void unregister_events()
{
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(on_destroy_list);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(on_reset);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_targets);
    reshade::unregister_event<reshade::addon_event::push_constants>(on_push_constants);
    reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
    reshade::unregister_event<reshade::addon_event::bind_descriptor_tables>(on_bind_tables);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::unregister_event<reshade::addon_event::init_resource>(on_init_resource);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
    descriptor_tracker::unregister_events();
}

const root_state *root_of(command_list *cmd, bool compute)
{
    const list_state *s = cmd != nullptr ? cmd->get_private_data<list_state>() : nullptr;
    return s != nullptr ? &s->roots[compute ? 1 : 0] : nullptr;
}

const targets *targets_of(command_list *cmd)
{
    const list_state *s = cmd != nullptr ? cmd->get_private_data<list_state>() : nullptr;
    return s != nullptr ? &s->bound : nullptr;
}

bool compute_hash(uint64_t pipeline, uint64_t &hash)
{
    std::shared_lock lock(g_pipelines_lock);
    const auto it = g_pipelines.find(pipeline);
    if (it == g_pipelines.end() || !it->second.compute)
        return false;
    hash = it->second.hash;
    return true;
}
} // namespace scene_state
