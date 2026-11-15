// Detailed logging (Debug=1 under [Diagnostics]). Every 10 seconds the log gets the frame times and the GPU time
// of each pass this add-on changes. It also counts how often the game ran our shader copies against its own.
//
// A pass is timed from the draw or dispatch right after its pipeline is bound to the next command recorded on
// the same list. Timestamps go into a query heap ring and are read RING - 2 frames later. A pass the game runs
// more than once a frame is timed the first time only.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include <reshade.hpp>

#include "diagnostics.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace reshade::api;

namespace
{
const uint32_t RING = 8;
const char *const NAMES[PASSES] = {"AO", "AO blur (horizontal)", "AO blur (vertical)", "reflections"};
// More GPU time than this a frame earns a warning.
const double TOO_SLOW_MS = 2.0;

bool g_on = false;
std::atomic<uint64_t> g_frame{0};

ID3D12QueryHeap *g_heap = nullptr;
ID3D12Resource *g_readback = nullptr;
double g_ticks_per_ms = 0.0;
// Per ring frame and pass: frame + 1 once started and once resolved.
std::atomic<uint64_t> g_started[RING][PASSES], g_resolved[RING][PASSES];

std::atomic<uint32_t> g_ours[PASSES], g_games[PASSES];

// The pass bound on this thread's list, waiting for its draw or dispatch, and the one being timed.
struct list_state
{
    command_list *cmd = nullptr;
    int bound = -1;
    int timing = -1;
    uint64_t frame = 0;
};
thread_local list_state t_list;

// The 10 second window.
std::chrono::steady_clock::time_point g_window_start, g_last_present;
std::vector<float> g_frame_ms;
double g_gpu_ms[PASSES] = {};
uint32_t g_gpu_frames[PASSES] = {};

uint32_t slot(uint64_t frame, int pass)
{
    return uint32_t(frame % RING) * PASSES * 2 + uint32_t(pass) * 2;
}

bool make_heap(device *dev)
{
    auto *d3d = reinterpret_cast<ID3D12Device *>(dev->get_native());
    D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, RING * PASSES * 2, 0};
    if (FAILED(d3d->CreateQueryHeap(&desc, IID_PPV_ARGS(&g_heap))))
        return false;
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = RING * PASSES * 2 * sizeof(uint64_t);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&g_readback))))
    {
        g_heap->Release();
        g_heap = nullptr;
        return false;
    }
    return true;
}

void stop_timing(command_list *cmd)
{
    if (t_list.cmd != cmd || t_list.timing < 0)
        return;
    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    const uint32_t first = slot(t_list.frame, t_list.timing);
    list->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
    list->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, first, 2, g_readback, first * sizeof(uint64_t));
    g_resolved[t_list.frame % RING][t_list.timing] = t_list.frame + 1;
    t_list.timing = -1;
}

// Before every draw and dispatch: ends the pass being timed and starts one that was just bound.
void on_command(command_list *cmd)
{
    if (t_list.cmd != cmd)
        return;
    stop_timing(cmd);
    if (t_list.bound < 0 || g_heap == nullptr)
        return;
    const int pass = t_list.bound;
    t_list.bound = -1;
    const uint64_t frame = g_frame.load();
    uint64_t expected = g_started[frame % RING][pass].load();
    if (expected == frame + 1 || !g_started[frame % RING][pass].compare_exchange_strong(expected, frame + 1))
        return;
    auto *list = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    list->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot(frame, pass));
    t_list.timing = pass;
    t_list.frame = frame;
}

bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t)
{
    on_command(cmd);
    return false;
}

bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
    on_command(cmd);
    return false;
}

bool on_dispatch(command_list *cmd, uint32_t, uint32_t, uint32_t)
{
    on_command(cmd);
    return false;
}

bool on_indirect(command_list *cmd, indirect_command, resource, uint64_t, uint32_t, uint32_t)
{
    on_command(cmd);
    return false;
}

void on_bind_pipeline(command_list *cmd, pipeline_stage, pipeline)
{
    if (t_list.cmd == cmd)
        stop_timing(cmd);
}

void read_frame(uint64_t frame)
{
    if (g_readback == nullptr || g_ticks_per_ms <= 0.0)
        return;
    void *data = nullptr;
    const D3D12_RANGE all = {0, RING * PASSES * 2 * sizeof(uint64_t)};
    if (FAILED(g_readback->Map(0, &all, &data)))
        return;
    const auto *ticks = static_cast<const uint64_t *>(data);
    for (int pass = 0; pass < PASSES; ++pass)
    {
        if (g_resolved[frame % RING][pass].load() != frame + 1)
            continue;
        const uint32_t first = slot(frame, pass);
        if (ticks[first + 1] < ticks[first])
            continue;
        g_gpu_ms[pass] += double(ticks[first + 1] - ticks[first]) / g_ticks_per_ms;
        ++g_gpu_frames[pass];
    }
    const D3D12_RANGE nothing = {0, 0};
    g_readback->Unmap(0, &nothing);
}

void write_window()
{
    char text[320];
    if (!g_frame_ms.empty())
    {
        std::vector<float> sorted = g_frame_ms;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (float ms : sorted)
            sum += ms;
        const float slowest = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
        snprintf(text, sizeof(text), "frame time: average %.2f ms, 1%% slowest from %.2f ms, worst %.2f ms (%zu frames)",
                 sum / double(sorted.size()), slowest, sorted.back(), sorted.size());
        dd_log(reshade::log::level::info, text);
    }
    int length = snprintf(text, sizeof(text), "GPU per frame:");
    for (int pass = 0; pass < PASSES; ++pass)
    {
        if (g_gpu_frames[pass] == 0)
        {
            length += snprintf(text + length, sizeof(text) - length, " %s not run.", NAMES[pass]);
            continue;
        }
        const double ms = g_gpu_ms[pass] / double(g_gpu_frames[pass]);
        length += snprintf(text + length, sizeof(text) - length, " %s %.3f ms.", NAMES[pass], ms);
        if (ms > TOO_SLOW_MS)
        {
            char warning[128];
            snprintf(warning, sizeof(warning), "%s took %.2f ms a frame. That's far more than expected.", NAMES[pass],
                     ms);
            dd_log(reshade::log::level::warning, warning);
        }
    }
    dd_log(reshade::log::level::info, text);
    length = snprintf(text, sizeof(text), "shaders run, ours against the game's:");
    for (int pass = 0; pass < PASSES; ++pass)
        length += snprintf(text + length, sizeof(text) - length, " %s %u / %u.", NAMES[pass], g_ours[pass].exchange(0),
                           g_games[pass].exchange(0));
    dd_log(reshade::log::level::info, text);
    g_frame_ms.clear();
    for (int pass = 0; pass < PASSES; ++pass)
    {
        g_gpu_ms[pass] = 0.0;
        g_gpu_frames[pass] = 0;
    }
}
} // namespace

void diagnostics_start(bool on)
{
    g_on = on;
    if (!on)
        return;
    reshade::register_event<reshade::addon_event::draw>(on_draw);
    reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
    reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
    reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
}

void diagnostics_stop()
{
    if (!g_on)
        return;
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
    reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
    reshade::unregister_event<reshade::addon_event::draw>(on_draw);
    if (g_readback != nullptr)
        g_readback->Release();
    if (g_heap != nullptr)
        g_heap->Release();
    g_readback = nullptr;
    g_heap = nullptr;
    g_on = false;
}

bool diagnostics_on()
{
    return g_on;
}

void diagnostics_bound(command_list *cmd, int pass, bool ours)
{
    if (!g_on)
        return;
    ++(ours ? g_ours : g_games)[pass];
    if (t_list.cmd == cmd)
        stop_timing(cmd);
    t_list.cmd = cmd;
    t_list.bound = pass;
}

void diagnostics_present(command_queue *queue)
{
    if (!g_on)
        return;
    if (g_heap == nullptr)
    {
        if (!make_heap(queue->get_device()))
        {
            dd_log(reshade::log::level::warning, "detailed logging: GPU timing could not be set up");
            g_on = false;
            return;
        }
        UINT64 frequency = 0;
        if (SUCCEEDED(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native())->GetTimestampFrequency(&frequency)))
            g_ticks_per_ms = double(frequency) / 1000.0;
        g_window_start = g_last_present = std::chrono::steady_clock::now();
        dd_log(reshade::log::level::info, "detailed logging on: frame and GPU times every 10 seconds");
    }
    const auto now = std::chrono::steady_clock::now();
    g_frame_ms.push_back(std::chrono::duration<float, std::milli>(now - g_last_present).count());
    g_last_present = now;
    const uint64_t frame = g_frame.fetch_add(1) + 1;
    if (frame >= RING)
        read_frame(frame - (RING - 2));
    if (now - g_window_start >= std::chrono::seconds(10))
    {
        write_window();
        g_window_start = now;
    }
}
