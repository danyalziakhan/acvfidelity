// The scene hook. See scene_inject.h for what it does and why.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include <reshade.hpp>

#include "scene_inject.h"
#include "descriptors.h"
#include "scene_state.h"
#include "light.h"
#include "light_patch.h"
#include "log.h"

#include "CSCapture.h"
#include "CSCaptureDown.h"
#include "CSDepthHalf.h"
#include "CSPrepare.h"
#include "CSPyramidDown.h"
#include "CSSetup.h"
#include "CSTemporal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace reshade::api;

namespace scene
{
namespace
{
// Our own shader-visible heap. The game's is at the device's limit (1000000) and the game writes all of it as a
// ring, so nothing can be added or borrowed there. Our passes and the scene-lighting draws that read our textures
// use this heap: the descriptors of every table such a draw uses are copied into it. It is used in SLICES frame
// slices, round robin. The GPU is never more than a few frames behind, so a slice is free again when its turn comes.
const uint32_t OUR_SIZE = 32768, SLICES = 4, SLICE = OUR_SIZE / SLICES;
const uint32_t FLAG_FIRST = 1, FLAG_EXPOSURE = 2, FLAG_CAPTURE = 4, FLAG_MOTION = 32, FLAG_GLOSS = 512;
// The game's AO history pass reads its motion vectors at space-4 t4 (see decode_motion in shaders/common.hlsli).
const uint64_t AO_HISTORY_PASS = 0xC9582762947B8EC4ull;
const float CUT_METERS = 5.0f;

std::atomic<bool> g_on{false};
std::atomic<bool> g_broken{false}; // turned off for good this session (logged)
device *g_device = nullptr;
ID3D12Device *g_native = nullptr;

// Once per message.
std::mutex g_log_lock;
std::set<std::string> g_logged;

void log_once(const std::string &text, bool warning = true)
{
    std::lock_guard lock(g_log_lock);
    if (!g_logged.insert(text).second)
        return;
    dd_log(warning ? reshade::log::level::warning : reshade::log::level::info, ("scene hook: " + text).c_str());
}

// ---------------------------------------------------------------- our heap

ID3D12DescriptorHeap *g_heap = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE g_heap_cpu = {};
D3D12_GPU_DESCRIPTOR_HANDLE g_heap_gpu = {};
uint32_t g_increment = 0;
std::atomic<uint64_t> g_present{0};
std::atomic<uint32_t> g_used{0}; // descriptors this frame took from its slice
std::atomic<uint32_t> g_users{0}; // bit per user that has the hook on (enable_for)

struct span
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(uint32_t i) const
    {
        return {cpu.ptr + SIZE_T(i) * g_increment};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_at(uint32_t i) const
    {
        return {gpu.ptr + UINT64(i) * g_increment};
    }
};

bool allocate(uint32_t count, span &out)
{
    if (g_heap == nullptr)
        return false;
    const uint32_t at = g_used.fetch_add(count);
    if (at + count > SLICE)
    {
        log_once("a frame needed more descriptors than its slice of our heap. Some draws kept the game's lighting");
        return false;
    }
    const uint32_t index = uint32_t(g_present.load() % SLICES) * SLICE + at;
    out.cpu = {g_heap_cpu.ptr + SIZE_T(index) * g_increment};
    out.gpu = {g_heap_gpu.ptr + UINT64(index) * g_increment};
    return true;
}

// ---------------------------------------------------------------- root signatures of the game

struct range_rec
{
    descriptor_type type;
    uint32_t offset, reg, space, count;
};
struct param_rec
{
    bool table = false;
    descriptor_type type = descriptor_type::sampler; // a root descriptor's type
    uint32_t reg = 0, space = 0;
    std::vector<range_rec> ranges;
};
std::shared_mutex g_layouts_lock;
std::unordered_map<uint64_t, std::vector<param_rec>> g_layouts;

void add_ranges(param_rec &p, uint32_t count, const descriptor_range *ranges, size_t stride)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto &r = *reinterpret_cast<const descriptor_range *>(reinterpret_cast<const uint8_t *>(ranges) + i * stride);
        p.ranges.push_back({r.type, r.binding, r.dx_register_index, r.dx_register_space, r.count});
    }
}

void on_init_layout(device *, uint32_t count, const pipeline_layout_param *params, pipeline_layout layout)
{
    std::vector<param_rec> recs(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const pipeline_layout_param &p = params[i];
        param_rec &r = recs[i];
        switch (p.type)
        {
        case pipeline_layout_param_type::descriptor_table:
            r.table = true;
            add_ranges(r, p.descriptor_table.count, p.descriptor_table.ranges, sizeof(descriptor_range));
            break;
        case pipeline_layout_param_type::descriptor_table_with_flags:
            r.table = true;
            add_ranges(r, p.descriptor_table_with_flags.count, p.descriptor_table_with_flags.ranges,
                       sizeof(descriptor_range_with_flags));
            break;
        case pipeline_layout_param_type::push_descriptors:
            r.type = p.push_descriptors.type;
            r.reg = p.push_descriptors.dx_register_index;
            r.space = p.push_descriptors.dx_register_space;
            break;
        case pipeline_layout_param_type::push_descriptors_with_ranges:
        case pipeline_layout_param_type::push_descriptors_with_ranges_and_flags:
            if (p.descriptor_table.count != 0 && p.descriptor_table.ranges != nullptr)
            {
                r.type = p.descriptor_table.ranges[0].type;
                r.reg = p.descriptor_table.ranges[0].dx_register_index;
                r.space = p.descriptor_table.ranges[0].dx_register_space;
            }
            break;
        default:
            break;
        }
    }
    std::unique_lock lock(g_layouts_lock);
    g_layouts[layout.handle] = std::move(recs);
}

bool is_srv(descriptor_type t)
{
    return t == descriptor_type::shader_resource_view || t == descriptor_type::buffer_shader_resource_view ||
           t == descriptor_type::texture_shader_resource_view;
}

// Where the lighting finds what we need, per root signature.
struct where
{
    long param = -1;      // root parameter
    uint32_t offset = 0;  // slot in its table
    bool table = false;   // false: a root descriptor
};
// A descriptor table parameter of the lighting's root signature.
struct table_param
{
    uint32_t param = 0;
    uint32_t size = 0;    // slots
    bool sampler = false; // in the sampler heap: re-set as it is
};
struct lighting_layout
{
    bool ok = false;
    const char *why = nullptr;
    where inputs[INPUT_COUNT];
    where cb2;            // b0 space 1
    long space2 = -1;     // the table with our two registers
    uint32_t slot14 = 0, slot15 = 0;
    long slot_gloss = -1; // its u0 (our gloss UAV), -1 when the table has no u0
    std::vector<table_param> tables;
    uint32_t copy_size = 0; // CBV/SRV/UAV slots of all tables together
};
std::mutex g_analysis_lock;
std::unordered_map<uint64_t, lighting_layout> g_analysis;

where find_srv(const std::vector<param_rec> &params, uint32_t space, uint32_t reg)
{
    where w;
    for (size_t i = 0; i < params.size() && w.param < 0; ++i)
    {
        const param_rec &p = params[i];
        if (!p.table && is_srv(p.type) && p.space == space && p.reg == reg)
            w.param = long(i);
        for (const range_rec &r : p.ranges)
            if (p.table && is_srv(r.type) && r.space == space && reg >= r.reg && (r.count == UINT32_MAX || reg - r.reg < r.count))
            {
                w.param = long(i);
                w.offset = r.offset + (reg - r.reg);
                w.table = true;
                break;
            }
    }
    return w;
}

lighting_layout analyse(uint64_t layout)
{
    {
        std::lock_guard lock(g_analysis_lock);
        const auto it = g_analysis.find(layout);
        if (it != g_analysis.end())
            return it->second;
    }
    lighting_layout l;
    std::vector<param_rec> params;
    {
        std::shared_lock lock(g_layouts_lock);
        const auto it = g_layouts.find(layout);
        if (it != g_layouts.end())
            params = it->second;
    }
    const uint32_t regs[INPUT_MOTION] = {10, 7, 8, 6, 0};
    for (uint32_t i = 0; i < INPUT_MOTION; ++i)
        l.inputs[i] = find_srv(params, 1, regs[i]);
    for (size_t i = 0; i < params.size() && l.cb2.param < 0; ++i)
    {
        const param_rec &p = params[i];
        if (!p.table && p.type == descriptor_type::constant_buffer && p.reg == 0 && p.space == 1)
            l.cb2.param = long(i);
        for (const range_rec &r : p.ranges)
            if (p.table && r.type == descriptor_type::constant_buffer && r.space == 1 && r.reg == 0)
            {
                l.cb2 = {long(i), r.offset, true};
                break;
            }
    }
    const where a = find_srv(params, 2, LIGHT_SSR2_REGISTER), b = find_srv(params, 2, LIGHT_SSIL_REGISTER);
    if (a.table && b.table && a.param == b.param)
    {
        l.space2 = a.param;
        l.slot14 = a.offset;
        l.slot15 = b.offset;
        for (const range_rec &r : params[size_t(a.param)].ranges)
            if (r.type == descriptor_type::unordered_access_view && r.space == 2 && r.reg <= LIGHT_SSR2_GLOSS_UAV &&
                (r.count == UINT32_MAX || LIGHT_SSR2_GLOSS_UAV - r.reg < r.count))
                l.slot_gloss = long(r.offset + (LIGHT_SSR2_GLOSS_UAV - r.reg));
    }
    bool unbounded = false, mixed = false;
    for (size_t i = 0; i < params.size(); ++i)
    {
        const param_rec &p = params[i];
        if (!p.table)
            continue;
        table_param t;
        t.param = uint32_t(i);
        int samplers = 0;
        for (const range_rec &r : p.ranges)
        {
            if (r.count == UINT32_MAX)
                unbounded = true;
            else
                t.size = std::max(t.size, r.offset + r.count);
            samplers += r.type == descriptor_type::sampler;
        }
        t.sampler = samplers != 0;
        mixed |= samplers != 0 && size_t(samplers) != p.ranges.size();
        if (!t.sampler)
            l.copy_size += t.size;
        l.tables.push_back(t);
    }
    if (params.empty())
        l.why = "the lighting's root signature was not seen";
    else if (l.space2 < 0)
        l.why = "the space-2 table does not reach t14 and t15";
    else if (unbounded || mixed)
        l.why = "a table of the lighting is unbounded or mixes samplers with views";
    else if (l.copy_size > 2048)
        l.why = "the lighting's tables are too large to copy";
    else if (l.cb2.param < 0)
        l.why = "the view constants (b0 space 1) were not found";
    else
    {
        l.ok = true;
        for (uint32_t i = 0; i < INPUT_EXPOSURE; ++i)
            if (l.inputs[i].param < 0 || !l.inputs[i].table)
            {
                l.ok = false;
                l.why = "a G-buffer texture is not in a table";
            }
    }
    std::lock_guard lock(g_analysis_lock);
    g_analysis[layout] = l;
    return l;
}

// ---------------------------------------------------------------- per command list

struct __declspec(uuid("8e0f3c52-17a4-4d6b-9b2e-5c1d7a90e3f4")) list_state
{
    uint64_t lighting = 0, ssr = 0, fallback = 0; // the game's lighting pipeline bound, its two copies
    uint64_t lighting_hash = 0;                   // its shader hash
    const char *missing = nullptr;                // why it has no SSR copy for good
    uint64_t current = 0;                         // the pipeline state really set on the list
    bool ssr_bound = false;
    bool ours_bound = false;   // our heap is set on the list (the game's tables are invalid until put back)
    ID3D12DescriptorHeap *game_heaps[2] = {};    // the game's CBV/SRV/UAV heap and sampler heap
    uint32_t game_heap_count = 0;
    bool lit = false;          // scene-lighting draws were recorded since the last capture
    uint64_t lit_target = 0;   // the lighting's render target resource
    struct seen
    {
        uint64_t res;
        uint32_t state;
    };
    std::vector<seen> states; // barriers recorded on this list, last state per resource

    void set_state(uint64_t res, uint32_t state)
    {
        for (seen &s : states)
            if (s.res == res)
            {
                s.state = state;
                return;
            }
        if (states.size() < 512)
            states.push_back({res, state});
    }
    bool state_of(uint64_t res, uint32_t &state) const
    {
        for (const seen &s : states)
            if (s.res == res)
            {
                state = s.state;
                return true;
            }
        return false;
    }
};
std::atomic<int> g_lists_on_ours{0}; // lists with our heap set: the restore checks skip the lookup when 0

list_state &state_of(command_list *cmd)
{
    if (list_state *s = cmd->get_private_data<list_state>())
        return *s;
    return *cmd->create_private_data<list_state>();
}

// A ReShade usage as a D3D12 state, for textures. False when it has no plain D3D12 meaning.
bool d3d12_state(resource_usage usage, uint32_t &state)
{
    const uint32_t u = static_cast<uint32_t>(usage);
    if ((u & 0x80000000u) != 0 || (u & 0x8000u) != 0 || u > 0xFFFFFu)
        return false;
    state = u;
    return true;
}

// ---------------------------------------------------------------- our objects

ID3D12RootSignature *g_root = nullptr;
std::mutex g_pipelines_lock;
std::unordered_map<const void *, ID3D12PipelineState *> g_pipelines;
ID3D12Resource *g_camera = nullptr; // 32 float4
std::vector<pass_fn> g_passes;
texture *g_outputs[2] = {};

texture g_depth[2], g_normals, g_pyramid, g_capture, g_motion;
// The lighting's own final gloss and a 1 where a patched variant drew (x, y of RGBA16F), written by the lighting into g_gloss[run & 1] while the previous frame's is read: the exact roughness and
// reflects mask for our passes.
texture g_gloss[2];
texture *g_gloss_now = nullptr;          // the one the lighting writes this frame
std::atomic<int> g_gloss_state{0};       // 0 not decided, 1 the lighting exports it, -1 no u0 in its table
// The game's motion vector target as last seen at its AO history pass, and when (present count).
std::atomic<uint64_t> g_motion_res{0};
std::atomic<uint64_t> g_motion_seen{0};
bool g_captured = false;   // the capture holds a frame (not since its last resize)
bool g_first = true;       // the next run has no history
uint64_t g_runs = 0;
std::atomic<uint64_t> g_ran_on{UINT64_MAX};   // present count of the last run
std::atomic<uint64_t> g_tried_on{UINT64_MAX}; // and of the last try

// This frame's game inputs (written by the run, read by its passes; same thread).
struct game_input
{
    bool copy = false;                        // copy the game's descriptor from `source`
    D3D12_CPU_DESCRIPTOR_HANDLE source = {};
    bool create = false;                      // or make a view of `res` with `desc`
    ID3D12Resource *res = nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
};
game_input g_inputs[INPUT_COUNT];
D3D12_GPU_VIRTUAL_ADDRESS g_cb2 = 0;
std::vector<texture *> g_touched;             // our textures out of their canonical state in this batch

D3D12_RESOURCE_STATES canonical(const texture &t)
{
    if (t.uav_home)
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return t.output ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                    : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

void end_batch(ID3D12GraphicsCommandList *list)
{
    std::vector<D3D12_RESOURCE_BARRIER> b;
    for (texture *t : g_touched)
        if (t->state != canonical(*t))
        {
            b.push_back(transition(t->res, t->state, canonical(*t)));
            t->state = canonical(*t);
        }
    g_touched.clear();
    if (!b.empty())
        list->ResourceBarrier(UINT(b.size()), b.data());
}

// The game's compute root state and the pipeline the list had, back after our passes. Tables are set only when the
// game's heap is on the list (otherwise put_game_heaps sets them).
void restore(command_list *cmd, ID3D12GraphicsCommandList *list, uint64_t pipeline, bool tables)
{
    if (pipeline != 0)
        list->SetPipelineState(reinterpret_cast<ID3D12PipelineState *>(pipeline));
    const scene_state::root_state *root = scene_state::root_of(cmd, true);
    if (root == nullptr || root->layout == 0)
        return;
    list->SetComputeRootSignature(reinterpret_cast<ID3D12RootSignature *>(root->layout));
    for (uint32_t i = 0; i < root->param_count; ++i)
    {
        const scene_state::root_value &v = root->params[i];
        const D3D12_GPU_VIRTUAL_ADDRESS address =
            v.buffer.buffer.handle != 0
                ? reinterpret_cast<ID3D12Resource *>(v.buffer.buffer.handle)->GetGPUVirtualAddress() + v.buffer.offset
                : v.buffer.offset;
        switch (v.kind)
        {
        case scene_state::root_kind::table:
            if (tables)
                list->SetComputeRootDescriptorTable(i, {v.table});
            break;
        case scene_state::root_kind::buffer:
            if (v.type == descriptor_type::constant_buffer)
                list->SetComputeRootConstantBufferView(i, address);
            else if (v.type == descriptor_type::buffer_unordered_access_view)
                list->SetComputeRootUnorderedAccessView(i, address);
            else
                list->SetComputeRootShaderResourceView(i, address);
            break;
        case scene_state::root_kind::constants:
            list->SetComputeRoot32BitConstants(i, v.constant_count, v.constants, 0);
            break;
        default:
            break;
        }
    }
}

// Our heap on the list, with the game's sampler heap beside it.
void use_our_heap(ID3D12GraphicsCommandList *list, list_state &ls)
{
    if (ls.ours_bound)
        return;
    ID3D12DescriptorHeap *heaps[2] = {g_heap, nullptr};
    UINT count = 1;
    for (uint32_t i = 0; i < ls.game_heap_count; ++i)
        if (ls.game_heaps[i] != nullptr &&
            ls.game_heaps[i]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
            heaps[count++] = ls.game_heaps[i];
    list->SetDescriptorHeaps(count, heaps);
    ls.ours_bound = true;
    ++g_lists_on_ours;
}

// The game's heaps back, and every table the game had set (graphics and compute), as the game set them.
void put_game_heaps(command_list *cmd, list_state &ls)
{
    if (!ls.ours_bound)
        return;
    ls.ours_bound = false;
    --g_lists_on_ours;
    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    if (ls.game_heap_count != 0)
        list->SetDescriptorHeaps(ls.game_heap_count, ls.game_heaps);
    for (int point = 0; point < 2; ++point)
    {
        const scene_state::root_state *root = scene_state::root_of(cmd, point == 1);
        if (root == nullptr || root->layout == 0)
            continue;
        for (uint32_t i = 0; i < root->param_count; ++i)
            if (root->params[i].kind == scene_state::root_kind::table)
            {
                if (point == 0)
                    list->SetGraphicsRootDescriptorTable(i, {root->params[i].table});
                else
                    list->SetComputeRootDescriptorTable(i, {root->params[i].table});
            }
    }
}

// Typeless formats as the float or unorm view the lighting target needs.
DXGI_FORMAT readable(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return f;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

void null_srv(D3D12_CPU_DESCRIPTOR_HANDLE at)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    g_native->CreateShaderResourceView(nullptr, &d, at);
}

void texture_srv(const texture &t, int mip, D3D12_CPU_DESCRIPTOR_HANDLE at)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = t.format;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MostDetailedMip = mip < 0 ? 0 : UINT(mip);
    d.Texture2D.MipLevels = mip < 0 ? UINT(-1) : 1;
    g_native->CreateShaderResourceView(t.res, &d, at);
}

void camera_srv(D3D12_CPU_DESCRIPTOR_HANDLE at)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.NumElements = 32;
    d.Buffer.StructureByteStride = 16;
    g_native->CreateShaderResourceView(g_camera, &d, at);
}

// ---------------------------------------------------------------- shared helpers (scene_inject.h)
} // namespace

bool make_texture(texture &t, DXGI_FORMAT format, uint32_t width, uint32_t height, uint32_t mips, bool output)
{
    if (t.res != nullptr && t.format == format && t.width == width && t.height == height && t.mips == mips &&
        t.output == output)
        return false;
    if (t.res != nullptr)
    {
        // Lists recorded earlier may still use it. Kept until the device goes rather than tracked per frame.
        static std::vector<ID3D12Resource *> old;
        old.push_back(t.res);
        t.res = nullptr;
    }
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = UINT16(mips);
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    t.format = format;
    t.width = width;
    t.height = height;
    t.mips = mips;
    t.output = output;
    t.state = canonical(t);
    if (FAILED(g_native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, t.state, nullptr,
                                                 IID_PPV_ARGS(&t.res))))
    {
        t.res = nullptr;
        g_broken = true;
        log_once("a texture of our passes could not be made. The hook is off");
    }
    return true;
}

ID3D12PipelineState *pipeline(const void *code, size_t size)
{
    std::lock_guard lock(g_pipelines_lock);
    const auto it = g_pipelines.find(code);
    if (it != g_pipelines.end())
        return it->second;
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = g_root;
    d.CS = {code, size};
    ID3D12PipelineState *p = nullptr;
    if (g_root == nullptr || FAILED(g_native->CreateComputePipelineState(&d, IID_PPV_ARGS(&p))))
    {
        p = nullptr;
        g_broken = true;
        log_once("a pipeline of our passes could not be made. The hook is off");
    }
    g_pipelines[code] = p;
    return p;
}

void add_pass(pass_fn fn)
{
    if (std::find(g_passes.begin(), g_passes.end(), fn) == g_passes.end())
        g_passes.push_back(fn);
}

void set_output(uint32_t slot, texture *t)
{
    if (slot < 2)
        g_outputs[slot] = t;
}

void dispatch(const frame &f, ID3D12PipelineState *pso, std::initializer_list<view> srvs,
              std::initializer_list<view> uavs, const uint32_t constants[16], uint32_t groups_x, uint32_t groups_y)
{
    if (pso == nullptr || g_broken)
        return;
    span tables;
    if (!allocate(16, tables))
        return;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    auto touch = [&](texture &t, D3D12_RESOURCE_STATES want) {
        if (t.state == want && want == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = t.res;
            barriers.push_back(b);
        }
        else if (t.state != want)
        {
            barriers.push_back(transition(t.res, t.state, want));
            t.state = want;
        }
        if (std::find(g_touched.begin(), g_touched.end(), &t) == g_touched.end())
            g_touched.push_back(&t);
    };
    static texture camera_tex; // the camera buffer as a "texture" for state tracking
    camera_tex.res = g_camera;
    bool camera_written = false;

    uint32_t k = 0;
    for (const view &v : uavs)
    {
        if (k >= 8)
            break;
        const D3D12_CPU_DESCRIPTOR_HANDLE at = tables.cpu_at(8 + k++);
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        if (v.kind == view::ours && v.tex != nullptr && v.tex->res != nullptr)
        {
            touch(*v.tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            d.Format = v.tex->format;
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipSlice = v.mip < 0 ? 0 : UINT(v.mip);
            g_native->CreateUnorderedAccessView(v.tex->res, nullptr, &d, at);
        }
        else if (v.kind == view::camera)
        {
            touch(camera_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            camera_written = true;
            d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            d.Buffer.NumElements = 32;
            d.Buffer.StructureByteStride = 16;
            g_native->CreateUnorderedAccessView(g_camera, nullptr, &d, at);
        }
        else
        {
            d.Format = DXGI_FORMAT_R32_FLOAT;
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_native->CreateUnorderedAccessView(nullptr, nullptr, &d, at);
        }
    }
    for (; k < 8; ++k)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_native->CreateUnorderedAccessView(nullptr, nullptr, &d, tables.cpu_at(8 + k));
    }
    k = 0;
    for (const view &v : srvs)
    {
        if (k >= 7)
            break;
        const D3D12_CPU_DESCRIPTOR_HANDLE at = tables.cpu_at(k++);
        if (v.kind == view::ours && v.tex != nullptr && v.tex->res != nullptr)
        {
            touch(*v.tex, canonical(*v.tex));
            texture_srv(*v.tex, v.mip, at);
        }
        else if (v.kind == view::game && g_inputs[v.in].copy)
            g_native->CopyDescriptorsSimple(1, at, g_inputs[v.in].source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        else if (v.kind == view::game && g_inputs[v.in].create)
            g_native->CreateShaderResourceView(g_inputs[v.in].res, &g_inputs[v.in].desc, at);
        else if (v.kind == view::game && v.in == INPUT_EXPOSURE)
        {
            // No exposure buffer: any structured view keeps the slot valid (the flag tells the shader).
            D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
            d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Buffer.NumElements = 32 * 4;
            d.Buffer.StructureByteStride = 4;
            if (!camera_written)
                touch(camera_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_native->CreateShaderResourceView(g_camera, &d, at);
        }
        else
            null_srv(at);
    }
    for (; k < 7; ++k)
        null_srv(tables.cpu_at(k));
    if (!camera_written)
    {
        touch(camera_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        camera_srv(tables.cpu_at(7));
    }
    else
        null_srv(tables.cpu_at(7));

    ID3D12GraphicsCommandList *list = f.list;
    if (!barriers.empty())
        list->ResourceBarrier(UINT(barriers.size()), barriers.data());
    list->SetPipelineState(pso);
    list->SetComputeRootSignature(g_root);
    list->SetComputeRoot32BitConstants(0, 16, constants, 0);
    list->SetComputeRootConstantBufferView(1, g_cb2 != 0 ? g_cb2 : g_camera->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2, tables.gpu);
    list->SetComputeRootDescriptorTable(3, tables.gpu_at(8));
    list->Dispatch(std::max(groups_x, 1u), std::max(groups_y, 1u), 1);
}

void temporal(const frame &f, const shared_set &s, texture &signal, texture &history_before, texture &history_now,
              texture &length_before, texture &length_now, uint32_t depth_mip, float min_blend, float max_length)
{
    uint32_t c[16] = {history_now.width, history_now.height, f.width, f.height};
    memcpy(&c[4], &min_blend, 4);
    memcpy(&c[5], &max_length, 4);
    c[8] = uint32_t(f.run);
    c[9] = f.flags;
    c[10] = depth_mip;
    dispatch(f, pipeline(CSTemporal, sizeof(CSTemporal)),
             {view::of(signal), view::of(history_before), view::of(length_before), view::of(*s.depth_now),
              view::of(*s.depth_before), view::of(*s.motion)},
             {view::of(history_now, 0), view::of(length_now, 0)}, c, (history_now.width + 7) / 8,
             (history_now.height + 7) / 8);
}

namespace
{
uint32_t mip_count(uint32_t w, uint32_t h)
{
    uint32_t n = 1;
    while ((w > 1 || h > 1) && n < 16)
    {
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
        ++n;
    }
    return n;
}

D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(uint64_t heap, uint32_t offset, bool &shader_visible)
{
    auto *h = reinterpret_cast<ID3D12DescriptorHeap *>(heap);
    const D3D12_DESCRIPTOR_HEAP_DESC d = h->GetDesc();
    shader_visible = (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
    return {h->GetCPUDescriptorHandleForHeapStart().ptr +
            SIZE_T(offset) * g_native->GetDescriptorHandleIncrementSize(d.Type)};
}

// The copy source of a game descriptor in a table slot. First where the game created it (a CPU-only heap, a valid
// copy source), if that slot still holds it. Else the slot of the game's shader-visible heap itself: the exact
// descriptor the GPU reads. D3D12 asks for CPU-only sources, but desktop drivers copy from either.
D3D12_CPU_DESCRIPTOR_HANDLE game_slot(descriptor_heap heap, uint32_t offset)
{
    const descriptor_tracker *tracker = descriptor_tracker::of(g_device);
    uint64_t origin = 0;
    uint32_t at = 0;
    descriptor_type type;
    bool visible = true;
    D3D12_CPU_DESCRIPTOR_HANDLE source = {};
    if (tracker != nullptr && tracker->origin(heap, offset, origin, at, type))
        source = heap_cpu(origin, at, visible);
    if (!visible)
        return source;
    return heap_cpu(heap.handle, offset, visible);
}

ID3D12Resource *resource_of_slot(descriptor_heap heap, uint32_t offset)
{
    const descriptor_tracker *tracker = descriptor_tracker::of(g_device);
    const resource_view v = tracker != nullptr ? tracker->view(heap, offset) : resource_view{0};
    if (v.handle == 0)
        return nullptr;
    return reinterpret_cast<ID3D12Resource *>(g_device->get_resource_from_view(v).handle);
}

bool make_objects()
{
    if (g_root != nullptr)
        return true;
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    if (FAILED(g_native->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))) ||
        !o.TypedUAVLoadAdditionalFormats)
    {
        g_broken = true;
        log_once("the GPU can't load RGBA16F from a UAV. The hook is off");
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = OUR_SIZE;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_native->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap))))
    {
        g_heap = nullptr;
        g_broken = true;
        log_once("our descriptor heap could not be made. The hook is off");
        return false;
    }
    g_heap_cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    g_heap_gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    g_increment = g_native->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (FAILED(g_native->CreateRootSignature(0, CSSetup, sizeof(CSSetup), IID_PPV_ARGS(&g_root))))
    {
        g_root = nullptr;
        g_broken = true;
        log_once("our root signature could not be made. The hook is off");
        return false;
    }
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 32 * 16;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(g_native->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                 IID_PPV_ARGS(&g_camera))))
    {
        g_camera = nullptr;
        g_broken = true;
        log_once("the camera block could not be made. The hook is off");
        return false;
    }
    char text[160];
    snprintf(text, sizeof(text), "our shader-visible heap of %u descriptors is made (%u a frame)", OUR_SIZE, SLICE);
    log_once(text, false);
    return true;
}

// Game resources our compute passes read: made readable from compute if this list's barriers say they are not.
struct readable_input
{
    ID3D12Resource *res;
    uint32_t before;
};

// The shared passes and every registered pass, once a frame, at a scene-lighting draw. False: nothing ran.
bool run(command_list *cmd, list_state &ls, const scene_state::root_state &root, const lighting_layout &l)
{
    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    if (list == nullptr || !make_objects())
        return false;
    // The view constants and the tables, as the lighting has them for this draw.
    if (uint32_t(l.cb2.param) >= root.param_count)
    {
        log_once("the lighting's view constants are not bound yet at its draw");
        return false;
    }
    const scene_state::root_value &cb = root.params[l.cb2.param];
    g_cb2 = 0;
    if (!l.cb2.table && cb.kind == scene_state::root_kind::buffer && cb.buffer.buffer.handle != 0)
        g_cb2 = reinterpret_cast<ID3D12Resource *>(cb.buffer.buffer.handle)->GetGPUVirtualAddress() + cb.buffer.offset;
    else if (l.cb2.table && cb.kind == scene_state::root_kind::table)
    {
        descriptor_heap heap = {0};
        uint32_t base = 0;
        g_device->get_descriptor_heap_offset({cb.table}, 0, 0, &heap, &base);
        const buffer_range r = descriptor_tracker::of(g_device)->buffer(heap, base + l.cb2.offset);
        if (r.buffer.handle != 0)
            g_cb2 = reinterpret_cast<ID3D12Resource *>(r.buffer.handle)->GetGPUVirtualAddress() + r.offset;
    }
    if (g_cb2 == 0)
    {
        log_once("the lighting's view constants could not be read");
        return false;
    }

    std::vector<readable_input> inputs;
    uint32_t width = 0, height = 0, albedo_w = 0, albedo_h = 0;
    for (uint32_t i = 0; i < INPUT_MOTION; ++i)
    {
        g_inputs[i] = {};
        const where &w = l.inputs[i];
        if (w.param < 0 || uint32_t(w.param) >= root.param_count)
            continue;
        const scene_state::root_value &v = root.params[w.param];
        if (w.table && v.kind == scene_state::root_kind::table)
        {
            descriptor_heap heap = {0};
            uint32_t base = 0;
            g_device->get_descriptor_heap_offset({v.table}, 0, 0, &heap, &base);
            g_inputs[i].copy = true;
            g_inputs[i].source = game_slot(heap, base + w.offset);
            if (i == INPUT_EXPOSURE)
                continue;
            ID3D12Resource *res = resource_of_slot(heap, base + w.offset);
            if (res == nullptr)
            {
                log_once("a G-buffer input of the lighting is not known to the descriptor tracker");
                return false;
            }
            const D3D12_RESOURCE_DESC d = res->GetDesc();
            if (i == INPUT_DEPTH)
            {
                width = uint32_t(d.Width);
                height = d.Height;
            }
            if (i == INPUT_ALBEDO)
            {
                albedo_w = uint32_t(d.Width);
                albedo_h = d.Height;
            }
            uint32_t state = 0;
            if (ls.state_of(reinterpret_cast<uint64_t>(res), state))
                inputs.push_back({res, state});
            else
                log_once("the state of a G-buffer texture is not known on the lighting's list. Read as it is", false);
        }
        else if (!w.table && i == INPUT_EXPOSURE && v.kind == scene_state::root_kind::buffer && v.buffer.buffer.handle != 0)
        {
            game_input &g = g_inputs[i];
            g.create = true;
            g.res = reinterpret_cast<ID3D12Resource *>(v.buffer.buffer.handle);
            g.desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            g.desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            g.desc.Buffer.FirstElement = v.buffer.offset / 4;
            g.desc.Buffer.NumElements = 1;
            g.desc.Buffer.StructureByteStride = 4;
        }
    }
    if (!g_inputs[INPUT_EXPOSURE].copy && !g_inputs[INPUT_EXPOSURE].create)
        log_once("the exposure buffer (s1 t0) was not found. Exposure changes between frames are not corrected", false);
    if (width == 0 || height == 0 || width > 16384 || height > 16384)
    {
        log_once("the depth buffer's size is not known");
        return false;
    }
    // The patched lighting reads our texture with its albedo read's uv: both must have one size.
    if (albedo_w != width || albedo_h != height)
    {
        char text[160];
        snprintf(text, sizeof(text), "the albedo (%ux%u) and the depth buffer (%ux%u) differ in size: our texture is "
                 "read at shifted pixels", albedo_w, albedo_h, width, height);
        log_once(text);
    }
    // The game's motion vectors, when its AO history pass ran this frame or the last one with a target of the
    // G-buffer's size and a format we read.
    g_inputs[INPUT_MOTION] = {};
    if (ID3D12Resource *m = reinterpret_cast<ID3D12Resource *>(g_motion_res.load());
        m != nullptr && g_present.load() - g_motion_seen.load() <= 1)
    {
        const D3D12_RESOURCE_DESC d = m->GetDesc();
        const DXGI_FORMAT f = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM
                              : d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                          : d.Format;
        const bool known = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
                           f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R16G16B16A16_UNORM;
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == width && d.Height == height && known)
        {
            game_input &g = g_inputs[INPUT_MOTION];
            g.create = true;
            g.res = m;
            g.desc.Format = f;
            g.desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            g.desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            g.desc.Texture2D.MipLevels = 1;
            uint32_t state = 0;
            if (ls.state_of(reinterpret_cast<uint64_t>(m), state))
                inputs.push_back({m, state});
        }
        else
            log_once("the game's motion vector target is not the G-buffer's size or a format we read. Camera motion only");
    }
    else
        log_once("the game's motion vectors were not seen this frame (AO history pass). Camera motion only", false);

    // Shared textures.
    bool fresh = false;
    fresh |= make_texture(g_depth[0], DXGI_FORMAT_R32_FLOAT, width, height, 2, false);
    fresh |= make_texture(g_depth[1], DXGI_FORMAT_R32_FLOAT, width, height, 2, false);
    fresh |= make_texture(g_normals, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, false);
    fresh |= make_texture(g_motion, DXGI_FORMAT_R16G16_FLOAT, width, height, 1, false);
    for (texture &g : g_gloss)
    {
        g.uav_home = true;
        make_texture(g, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, false);
    }
    const uint32_t mips = mip_count(width, height);
    fresh |= make_texture(g_pyramid, DXGI_FORMAT_R32_FLOAT, width, height, mips, false);
    if (g_broken)
        return false;
    if (fresh)
        g_first = true;

    // Game inputs readable from compute for the batch.
    std::vector<D3D12_RESOURCE_BARRIER> to, back;
    for (const readable_input &r : inputs)
        if ((r.before & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0)
        {
            if ((r.before & (D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_RENDER_TARGET |
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST)) != 0)
            {
                log_once("a G-buffer texture is in a write state at the lighting");
                return false;
            }
            const auto s = D3D12_RESOURCE_STATES(r.before);
            to.push_back(transition(r.res, s, s | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            back.push_back(transition(r.res, s | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, s));
        }
    if (!to.empty())
        list->ResourceBarrier(UINT(to.size()), to.data());
    use_our_heap(list, ls);

    frame f;
    f.list = list;
    f.run = g_runs;
    f.width = width;
    f.height = height;
    f.pyramid_mips = mips;
    f.flags = (g_first ? FLAG_FIRST : 0) |
              (g_inputs[INPUT_EXPOSURE].copy || g_inputs[INPUT_EXPOSURE].create ? FLAG_EXPOSURE : 0) |
              (g_captured ? FLAG_CAPTURE : 0) | (g_inputs[INPUT_MOTION].create ? FLAG_MOTION : 0) |
              (g_gloss_state.load() == 1 && !g_first ? FLAG_GLOSS : 0);
    texture &gloss_before = g_gloss[(g_runs + 1) & 1];
    g_gloss_now = &g_gloss[g_runs & 1];
    texture &depth_now = g_depth[g_runs & 1], &depth_before = g_depth[(g_runs + 1) & 1];
    const uint32_t gx = (width + 7) / 8, gy = (height + 7) / 8;

    uint32_t c[16] = {};
    c[8] = uint32_t(g_runs);
    c[9] = f.flags;
    memcpy(&c[4], &CUT_METERS, 4);
    dispatch(f, pipeline(CSSetup, sizeof(CSSetup)), {view::from(INPUT_EXPOSURE)}, {view::cam()}, c, 1, 1);
    c[0] = width;
    c[1] = height;
    dispatch(f, pipeline(CSPrepare, sizeof(CSPrepare)),
             {view::from(INPUT_DEPTH), view::from(INPUT_NORMAL), view::from(INPUT_MATERIAL), view::from(INPUT_ALBEDO),
              view::from(INPUT_MOTION)},
             {view::of(depth_now, 0), view::of(g_normals, 0), view::of(g_pyramid, 0), view::of(g_motion, 0),
              view::of(gloss_before, 0), view::of(*g_gloss_now, 0)},
             c, gx, gy);
    c[2] = std::max(width / 2, 1u);
    c[3] = std::max(height / 2, 1u);
    dispatch(f, pipeline(CSDepthHalf, sizeof(CSDepthHalf)), {}, {view::of(depth_now, 0), view::of(depth_now, 1)}, c,
             (c[2] + 7) / 8, (c[3] + 7) / 8);
    uint32_t w = width, h = height;
    for (uint32_t m = 1; m < mips; ++m)
    {
        const uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
        uint32_t p[16] = {w, h, nw, nh};
        p[8] = c[8];
        p[9] = c[9];
        dispatch(f, pipeline(CSPyramidDown, sizeof(CSPyramidDown)), {},
                 {view::of(g_pyramid, int(m - 1)), view::of(g_pyramid, int(m))}, p, (nw + 7) / 8, (nh + 7) / 8);
        w = nw;
        h = nh;
    }
    const shared_set s = {&depth_now, &depth_before, &g_normals, &g_pyramid, &g_capture, &g_motion, &gloss_before};
    for (pass_fn fn : g_passes)
        fn(f, s);
    end_batch(list);
    if (!back.empty())
        list->ResourceBarrier(UINT(back.size()), back.data());
    restore(cmd, list, ls.current, false);
    g_first = false;
    ++g_runs;
    return !g_broken;
}

// The lighting target into the capture, after the frame's scene lighting on this list.
void capture(command_list *cmd, list_state &ls)
{
    ls.lit = false;
    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    auto *target = reinterpret_cast<ID3D12Resource *>(ls.lit_target);
    if (list == nullptr || target == nullptr || g_root == nullptr || g_broken)
        return;
    const D3D12_RESOURCE_DESC d = target->GetDesc();
    const DXGI_FORMAT format = readable(d.Format);
    if (format == DXGI_FORMAT_UNKNOWN || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1)
    {
        log_once("the lighting target's format can't be read for the capture");
        return;
    }
    uint32_t state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ls.state_of(ls.lit_target, state);
    if ((state & (D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST |
                  D3D12_RESOURCE_STATE_DEPTH_WRITE)) != 0)
    {
        log_once("the lighting target is being written at the capture point");
        return;
    }
    const uint32_t w = uint32_t(d.Width), h = d.Height, mips = std::min(mip_count(w, h), 6u);
    if (make_texture(g_capture, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, mips, false))
        g_captured = false;
    if (g_broken)
        return;

    const auto s = D3D12_RESOURCE_STATES(state);
    const bool move = (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0;
    const auto readable_state =
        (state & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE)) != 0
            ? s | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (move)
    {
        const D3D12_RESOURCE_BARRIER b = transition(target, s, readable_state);
        list->ResourceBarrier(1, &b);
    }
    // The target as an input of the capture pass (in the depth input's place for this batch).
    game_input lit;
    lit.create = true;
    lit.res = target;
    lit.desc.Format = format;
    lit.desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    lit.desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lit.desc.Texture2D.MipLevels = 1;
    const game_input saved = g_inputs[INPUT_DEPTH];
    g_inputs[INPUT_DEPTH] = lit;
    use_our_heap(list, ls);

    frame f;
    f.list = list;
    f.width = w;
    f.height = h;
    uint32_t c[16] = {w, h};
    // CSCapture writes ColourTarget, u1 (scene.hlsl: u0 is the mip source of CSCaptureDown). Bound at u0 the capture
    // was never written and stayed all zero (measured live: 0 texels > 0).
    dispatch(f, pipeline(CSCapture, sizeof(CSCapture)), {view::from(INPUT_DEPTH)}, {view(), view::of(g_capture, 0)}, c,
             (w + 7) / 8, (h + 7) / 8);
    uint32_t mw = w, mh = h;
    for (uint32_t m = 1; m < mips; ++m)
    {
        const uint32_t nw = std::max(mw / 2, 1u), nh = std::max(mh / 2, 1u);
        uint32_t p[16] = {mw, mh, nw, nh};
        dispatch(f, pipeline(CSCaptureDown, sizeof(CSCaptureDown)), {},
                 {view::of(g_capture, int(m - 1)), view::of(g_capture, int(m))}, p, (nw + 7) / 8, (nh + 7) / 8);
        mw = nw;
        mh = nh;
    }
    end_batch(list);
    g_inputs[INPUT_DEPTH] = saved;
    if (move)
    {
        const D3D12_RESOURCE_BARRIER b = transition(target, readable_state, s);
        list->ResourceBarrier(1, &b);
    }
    restore(cmd, list, ls.current, false);
    put_game_heaps(cmd, ls);
    g_captured = !g_broken;
}

// This frame's copies of the lighting's tables in our heap, by game table and parameter. Many lighting draws of a
// frame use the same tables.
struct table_copy
{
    uint64_t game;
    uint32_t param;
    D3D12_GPU_DESCRIPTOR_HANDLE ours;
};
std::vector<table_copy> g_copies;
uint64_t g_copies_frame = UINT64_MAX;

// Every CBV/SRV/UAV table of the lighting draw copied to our heap (our two textures at t14 and t15 in the space-2
// one), our heap set, every table parameter set to our copy (sampler tables as the game had them).
bool use_our_tables(command_list *cmd, list_state &ls, const scene_state::root_state &root, const lighting_layout &l)
{
    const uint64_t now = g_present.load();
    if (g_copies_frame != now)
    {
        g_copies.clear();
        g_copies_frame = now;
    }
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> handles(l.tables.size());
    for (size_t k = 0; k < l.tables.size(); ++k)
    {
        const table_param &t = l.tables[k];
        if (t.param >= root.param_count || root.params[t.param].kind != scene_state::root_kind::table)
        {
            log_once("a table of the lighting is not set at its draw. Those draws keep the game's lighting");
            return false;
        }
        const uint64_t game = root.params[t.param].table;
        handles[k] = {game};
        if (t.sampler)
            continue;
        const auto it = std::find_if(g_copies.begin(), g_copies.end(),
                                     [&](const table_copy &c) { return c.game == game && c.param == t.param; });
        if (it != g_copies.end())
        {
            handles[k] = it->ours;
            continue;
        }
        span ours;
        if (!allocate(t.size, ours))
            return false;
        descriptor_heap heap = {0};
        uint32_t base = 0;
        g_device->get_descriptor_heap_offset({game}, 0, 0, &heap, &base);
        for (uint32_t slot = 0; slot < t.size; ++slot)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE at = ours.cpu_at(slot);
            if (long(t.param) == l.space2 && long(slot) == l.slot_gloss && g_gloss_state.load() == 1 &&
                g_gloss_now != nullptr && g_gloss_now->res != nullptr)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
                d.Format = g_gloss_now->format;
                d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                g_native->CreateUnorderedAccessView(g_gloss_now->res, nullptr, &d, at);
            }
            else if (long(t.param) == l.space2 && (slot == l.slot14 || slot == l.slot15))
            {
                const texture *tex = g_outputs[slot == l.slot14 ? 0 : 1];
                if (tex != nullptr && tex->res != nullptr)
                    texture_srv(*tex, -1, at);
                else
                    null_srv(at);
            }
            else
                g_native->CopyDescriptorsSimple(1, at, game_slot(heap, base + slot),
                                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        handles[k] = ours.gpu;
        g_copies.push_back({game, t.param, ours.gpu});
    }
    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    use_our_heap(list, ls);
    for (size_t k = 0; k < l.tables.size(); ++k)
        list->SetGraphicsRootDescriptorTable(l.tables[k].param, handles[k]);
    return true;
}

// The game's CBV/SRV/UAV and sampler heaps, as the tables bound on this list (graphics and compute) show them.
// No sampler table bound anywhere on the list: the game is taken to use static samplers only (logged once).
void learn_game_heaps(command_list *cmd, list_state &ls)
{
    ID3D12DescriptorHeap *found[2] = {};
    for (int point = 0; point < 2; ++point)
    {
        const scene_state::root_state *root = scene_state::root_of(cmd, point == 1);
        if (root == nullptr || root->layout == 0)
            continue;
        for (uint32_t i = 0; i < root->param_count; ++i)
        {
            if (root->params[i].kind != scene_state::root_kind::table || root->params[i].table == 0)
                continue;
            descriptor_heap heap = {0};
            uint32_t base = 0;
            g_device->get_descriptor_heap_offset({root->params[i].table}, 0, 0, &heap, &base);
            auto *h = reinterpret_cast<ID3D12DescriptorHeap *>(heap.handle);
            if (h == nullptr)
                continue;
            const bool sampler = h->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            if (found[sampler ? 1 : 0] == nullptr)
                found[sampler ? 1 : 0] = h;
        }
    }
    if (found[1] == nullptr)
        log_once("no sampler table is bound on the lighting's list. Only the game's view heap is put back after our "
                 "draws",
                 false);
    ls.game_heap_count = 0;
    for (ID3D12DescriptorHeap *h : found)
        if (h != nullptr)
            ls.game_heaps[ls.game_heap_count++] = h;
}

// ---------------------------------------------------------------- events

void on_init_device(device *dev)
{
    if (dev->get_api() != device_api::d3d12 || g_native != nullptr)
        return;
    g_device = dev;
    g_native = reinterpret_cast<ID3D12Device *>(dev->get_native());
}

void on_destroy_device(device *dev)
{
    if (dev != g_device)
        return;
    g_broken = true; // our objects belonged to it
    g_device = nullptr;
}

void unbind_ours(command_list *cmd, list_state &ls)
{
    put_game_heaps(cmd, ls);
    if (!ls.ssr_bound)
        return;
    ls.ssr_bound = false;
    ls.current = ls.fallback;
    reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native())
        ->SetPipelineState(reinterpret_cast<ID3D12PipelineState *>(ls.fallback));
}

void on_bind_pipeline(command_list *cmd, pipeline_stage, reshade::api::pipeline pipe)
{
    if (!g_on.load(std::memory_order_relaxed))
        return;
    list_state &ls = state_of(cmd);
    ls.current = pipe.handle;
    ls.ssr_bound = false;
    uint64_t ssr = 0, fallback = 0, hash = 0;
    const char *missing = nullptr;
    if (pipe.handle != 0 && light_ssr_pipelines(pipe.handle, &ssr, &fallback, &hash, &missing))
    {
        ls.lighting = pipe.handle;
        ls.lighting_hash = hash;
        ls.missing = missing;
        ls.ssr = ssr;
        ls.fallback = fallback;
        ls.current = fallback; // light.cpp has bound it already
    }
    else
        ls.lighting = 0;
}

std::mutex g_record_lock; // our passes and our textures are recorded by one list at a time

void lighting_draw(command_list *cmd, list_state &ls)
{
    const scene_state::root_state *root = scene_state::root_of(cmd, false);
    const scene_state::targets *targets = scene_state::targets_of(cmd);
    if (root == nullptr || root->layout == 0)
        return;
    if (targets != nullptr && targets->count != 0 && targets->rtv[0].handle != 0)
    {
        ls.lit = true;
        ls.lit_target = g_device->get_resource_from_view(targets->rtv[0]).handle;
        ls.set_state(ls.lit_target, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (g_broken)
    {
        unbind_ours(cmd, ls);
        return;
    }
    const lighting_layout l = analyse(root->layout);
    if (!l.ok)
    {
        log_once(l.why != nullptr ? l.why : "the lighting's layout is not as expected");
        unbind_ours(cmd, ls);
        return;
    }
    if (!ls.ours_bound)
        learn_game_heaps(cmd, ls);
    // The gloss export needs u0 in the space-2 table (the patched copies declare it).
    if (g_gloss_state.load() == 0)
    {
        if (l.slot_gloss >= 0)
        {
            g_gloss_state = 1;
            light_set_ssr2_gloss(true);
            log_once("the lighting's space-2 table has u0: the SSR copies export the game's final gloss", false);
        }
        else
        {
            g_gloss_state = -1;
            log_once("the lighting's space-2 table has no u0: no gloss export. Roughness from t8.y, every lit pixel "
                     "reflects");
        }
    }
    if (ls.ssr == 0)
    {
        // The copy may have been made since the pipeline was bound.
        uint64_t ssr = 0, fallback = 0, hash = 0;
        const char *missing = nullptr;
        if (light_ssr_pipelines(ls.lighting, &ssr, &fallback, &hash, &missing))
        {
            ls.ssr = ssr;
            ls.missing = missing;
        }
    }
    if (ls.game_heap_count == 0 || ls.game_heaps[0]->GetDesc().Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
    {
        log_once("the game's view heap is not known at the lighting");
        unbind_ours(cmd, ls);
        return;
    }
    const uint64_t now = g_present.load();
    if (g_tried_on.load() != now)
    {
        g_tried_on = now; // one try a frame
        if (run(cmd, ls, *root, l))
            g_ran_on = now;
    }
    if (g_ran_on.load() == now && ls.ssr != 0 && use_our_tables(cmd, ls, *root, l))
    {
        if (!ls.ssr_bound || ls.current != ls.ssr)
            reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native())
                ->SetPipelineState(reinterpret_cast<ID3D12PipelineState *>(ls.ssr));
        ls.ssr_bound = true;
        ls.current = ls.ssr;
        return;
    }
    unbind_ours(cmd, ls);
}

// Before any other GPU work on a list that has our heap: the game's heaps and tables back.
void game_work(command_list *cmd)
{
    if (g_lists_on_ours.load(std::memory_order_relaxed) == 0)
        return;
    if (list_state *ls = cmd->get_private_data<list_state>(); ls != nullptr && ls->ours_bound)
        put_game_heaps(cmd, *ls);
}

bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t)
{
    if (g_on.load(std::memory_order_relaxed))
    {
        list_state &ls = state_of(cmd);
        if (ls.lighting != 0)
        {
            std::lock_guard lock(g_record_lock);
            lighting_draw(cmd, ls);
            return false;
        }
    }
    game_work(cmd);
    return false;
}

bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
    return on_draw(cmd, 0, 0, 0, 0);
}

bool on_indirect(command_list *cmd, indirect_command type, resource, uint64_t, uint32_t, uint32_t)
{
    if (type == indirect_command::dispatch)
    {
            game_work(cmd);
        return false;
    }
    return on_draw(cmd, 0, 0, 0, 0);
}

// At the game's AO history pass: the target it reads its motion vectors from (space-4 t4 there).
void find_motion(command_list *cmd)
{
    const scene_state::root_state *root = scene_state::root_of(cmd, true);
    uint64_t hash = 0;
    if (root == nullptr || root->pipeline == 0 || !scene_state::compute_hash(root->pipeline, hash) || hash != AO_HISTORY_PASS)
        return;
    std::vector<param_rec> params;
    {
        std::shared_lock lock(g_layouts_lock);
        const auto it = g_layouts.find(root->layout);
        if (it != g_layouts.end())
            params = it->second;
    }
    const where w = find_srv(params, 4, 4);
    if (!w.table || uint32_t(w.param) >= root->param_count || root->params[w.param].kind != scene_state::root_kind::table)
    {
        log_once("the AO history pass has no motion vector table where expected");
        return;
    }
    descriptor_heap heap = {0};
    uint32_t base = 0;
    g_device->get_descriptor_heap_offset({root->params[w.param].table}, 0, 0, &heap, &base);
    ID3D12Resource *res = resource_of_slot(heap, base + w.offset);
    if (res == nullptr)
        return;
    if (g_motion_res.exchange(reinterpret_cast<uint64_t>(res)) != reinterpret_cast<uint64_t>(res))
    {
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        char text[160];
        snprintf(text, sizeof(text), "the game's motion vectors: %llux%u, DXGI format %d", static_cast<unsigned long long>(d.Width),
                 d.Height, int(d.Format));
        log_once(text, false);
    }
    g_motion_seen = g_present.load();
}

bool on_dispatch(command_list *cmd, uint32_t, uint32_t, uint32_t)
{
    if (g_on.load(std::memory_order_relaxed))
        find_motion(cmd);
    game_work(cmd);
    return false;
}

// The game set a table (or a root signature) while our heap was on the list: its heaps back first, then all tables.
void on_bind_tables(command_list *cmd, shader_stage, pipeline_layout, uint32_t, uint32_t, const descriptor_table *,
                    uint32_t, const uint32_t *)
{
    game_work(cmd);
}

bool on_clear_uav_uint(command_list *cmd, resource_view, const uint32_t[4], uint32_t, const rect *)
{
    game_work(cmd);
    return false;
}

bool on_clear_uav_float(command_list *cmd, resource_view, const float[4], uint32_t, const rect *)
{
    game_work(cmd);
    return false;
}

void on_barrier(command_list *cmd, uint32_t count, const resource *resources, const resource_usage *,
                const resource_usage *after)
{
    if (!g_on.load(std::memory_order_relaxed))
        return;
    list_state &ls = state_of(cmd);
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t s = 0;
        if (d3d12_state(after[i], s))
            ls.set_state(resources[i].handle, s);
    }
}

void on_bind_targets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view)
{
    if (!g_on.load(std::memory_order_relaxed))
        return;
    list_state &ls = state_of(cmd);
    if (!ls.lit)
        return;
    for (uint32_t i = 0; i < count; ++i)
        if (rtvs != nullptr && rtvs[i].handle != 0 && g_device->get_resource_from_view(rtvs[i]).handle == ls.lit_target)
            return;
    std::lock_guard lock(g_record_lock);
    capture(cmd, ls);
}

void on_close(command_list *cmd)
{
    if (!g_on.load(std::memory_order_relaxed))
    {
        game_work(cmd);
        return;
    }
    list_state &ls = state_of(cmd);
    if (ls.lit)
    {
        std::lock_guard lock(g_record_lock);
        capture(cmd, ls);
    }
    game_work(cmd);
}

void on_reset(command_list *cmd)
{
    if (list_state *s = cmd->get_private_data<list_state>())
    {
        if (s->ours_bound)
            --g_lists_on_ours;
        s->lighting = s->ssr = s->fallback = s->current = 0;
        s->ssr_bound = s->lit = s->ours_bound = false;
        s->states.clear();
    }
}

void on_destroy_list(command_list *cmd)
{
    if (list_state *s = cmd->get_private_data<list_state>(); s != nullptr && s->ours_bound)
        --g_lists_on_ours;
    cmd->destroy_private_data<list_state>();
}
} // namespace

void register_events()
{
    reshade::register_event<reshade::addon_event::init_device>(on_init_device);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::register_event<reshade::addon_event::init_pipeline_layout>(on_init_layout);
    reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_bind_tables);
    reshade::register_event<reshade::addon_event::barrier>(on_barrier);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_targets);
    reshade::register_event<reshade::addon_event::draw>(on_draw);
    reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
    reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
    reshade::register_event<reshade::addon_event::clear_unordered_access_view_uint>(on_clear_uav_uint);
    reshade::register_event<reshade::addon_event::clear_unordered_access_view_float>(on_clear_uav_float);
    reshade::register_event<reshade::addon_event::close_command_list>(on_close);
    reshade::register_event<reshade::addon_event::reset_command_list>(on_reset);
    reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_list);
}

void unregister_events()
{
    g_on = false;
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(on_destroy_list);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(on_reset);
    reshade::unregister_event<reshade::addon_event::close_command_list>(on_close);
    reshade::unregister_event<reshade::addon_event::clear_unordered_access_view_float>(on_clear_uav_float);
    reshade::unregister_event<reshade::addon_event::clear_unordered_access_view_uint>(on_clear_uav_uint);
    reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch);
    reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
    reshade::unregister_event<reshade::addon_event::draw>(on_draw);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_targets);
    reshade::unregister_event<reshade::addon_event::barrier>(on_barrier);
    reshade::unregister_event<reshade::addon_event::bind_descriptor_tables>(on_bind_tables);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::unregister_event<reshade::addon_event::init_pipeline_layout>(on_init_layout);
    reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
}

void present()
{
    g_used = 0;
    g_present.fetch_add(1);
}

void enable(bool on)
{
    enable_for(USER_SSR2, on);
}

void enable_for(uint32_t user, bool on)
{
    const uint32_t bit = 1u << user;
    const uint32_t before = on ? g_users.fetch_or(bit) : g_users.fetch_and(~bit);
    const uint32_t after = on ? before | bit : before & ~bit;
    g_on = after != 0;
    // The surface reflections relearn the gloss export each time it is switched (as when it was the only user).
    if (user == USER_SSR2 || before == 0 || after == 0)
        g_gloss_state = 0;
    if (on && (user == USER_SSR2 || before == 0))
        g_first = true;
}

bool enabled()
{
    return g_on.load() && !g_broken.load();
}
} // namespace scene
