// The game creates its 528 scene lighting pipelines at start. About 26 draw
// in a frame. A worker thread makes a copy only for a variant the game draws.
// It does so the first time the game binds it after a change. Until the copy exists
// the game's own pipeline (or the previous copy) stays bound. A copy is bound
// right after the game binds the exact pipeline it was made from.
//
// Replaced copies wait RETIRE_FRAMES frames before they are destroyed.
// Recorded command lists may still use them. All copies of a pipeline go when
// the game destroys it. All copies of a device go with the device.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include <reshade.hpp>

#include "light.h"
#include "light_patch.h"
#include "log.h"
#include "pipeline_copy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace reshade::api;

namespace
{
const uint64_t RETIRE_FRAMES = 16;
const uint64_t FNV_BASIS = 1469598103934665603ull;
const uint64_t FNV_PRIME = 1099511628211ull;

uint64_t fnv(uint64_t hash, const void *data, size_t size)
{
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * FNV_PRIME;
    return hash;
}

struct variant
{
    uint64_t game = 0;
    uint64_t hash = 0; // FNV-1a of the vertex and pixel code, as the debug tools name shaders
    device *dev = nullptr;
    pipeline_layout layout = {0};
    stored_desc desc;
    std::atomic<uint64_t> active{0}; // 0: the game's own
    std::atomic<uint32_t> built{0};  // the generation of values the active pipeline is for
    std::atomic<bool> queued{false};
    std::atomic<bool> failed{false}; // the game's own for good
    // A second copy that also reads our reflection and indirect light textures. Only the scene hook binds it, right
    // before a draw whose space-2 table it has replaced with its own.
    std::atomic<uint64_t> ssr_active{0};
    std::atomic<bool> ssr_failed{false};
    bool ssr_only = false; // the AO edits can't go in: only the copy with our reflection
    const char *ssr2_missing = nullptr; // why our patch can't go in (a literal), or nullptr
    const char *ssil_missing = nullptr; // why the indirect light patch can't go in, or nullptr
    // Under g_lock.
    bool dead = false;
    std::vector<uint64_t> made; // every copy not destroyed yet
};

// Game pipeline -> variant. The bind event reads it under a shared lock.
std::shared_mutex g_map_lock;
std::unordered_map<uint64_t, std::shared_ptr<variant>> g_map;
std::atomic<size_t> g_variants{0};

// Values, copies and counts.
std::mutex g_lock;
light_values g_values;
bool g_values_set = false;
std::atomic<uint32_t> g_gen{1};
std::atomic<bool> g_need{false};
struct retired_copy
{
    std::shared_ptr<variant> owner;
    uint64_t handle;
    uint64_t frame; // destroyed after this frame
};
std::vector<retired_copy> g_retired;
std::atomic<uint64_t> g_frame{0};
uint32_t g_ssr2 = 0; // under g_lock: the space-2 register of the SSR texture, 0 off
bool g_ssr2_gloss = false; // under g_lock: the SSR copies also export their final gloss
uint32_t g_ssil = 0; // under g_lock: the space-2 register of the indirect light texture, 0 off

// The worker. It holds a reference on the add-on while it runs and ends after two idle seconds.
std::mutex g_queue_lock;
std::condition_variable g_queue_wake;
std::deque<std::shared_ptr<variant>> g_queue;
// Workers: up to MAX_WORKERS build copies at once. After a change every variant on screen needs a new copy.
// The driver takes a while for each. One worker made the change show over several seconds.
const int MAX_WORKERS = 4;
int g_workers = 0;
bool g_stop = false;
// Held while a copy is made, so a device can't go away in the middle of it.
// Builds hold it shared, so several run at once. Device teardown takes it alone and so waits for them.
std::shared_mutex g_work_lock;
thread_local bool t_creating = false;

// Logging, under g_lock.
std::set<std::pair<uint64_t, std::string>> g_logged;
std::map<std::string, int> g_failures, g_partial;
int g_patched = 0, g_copies = 0;
std::unordered_set<uint64_t> g_refused;
bool g_summary_done = false;
std::chrono::steady_clock::time_point g_first_seen;

light_values from_ao(const float *v)
{
    light_values l;
    l.blend = v[AO_BLEND];
    l.sun = v[AO_SUN];
    l.sun_power = v[AO_SUN_POWER];
    l.bounce = v[AO_BOUNCE];
    l.sky = v[AO_SKY];
    l.sky_night = v[AO_SKY_NIGHT];
    l.contact = v[AO_CONTACT];
    return l;
}

bool same(const light_values &a, const light_values &b)
{
    return a.blend == b.blend && a.sun == b.sun && a.sun_power == b.sun_power && a.bounce == b.bounce &&
           a.sky == b.sky && a.sky_night == b.sky_night && a.contact == b.contact;
}

// Under g_lock. Once per shader and message. Left alone: `why` failed. Otherwise `what` lists the options
// the shader can't take.
void report(uint64_t hash, size_t size, const char *what, const char *why, bool alone)
{
    if (alone)
        ++g_failures[why];
    if (!g_logged.insert({hash, std::string(what) + why}).second)
        return;
    char text[512];
    if (alone)
        snprintf(text, sizeof(text),
                 "Lighting: shader %016llX (FNV-1a of vertex and pixel code, pixel shader %zu bytes) left alone: %s",
                 static_cast<unsigned long long>(hash), size, why);
    else
        snprintf(text, sizeof(text),
                 "Lighting: shader %016llX (FNV-1a of vertex and pixel code, pixel shader %zu bytes) patched "
                 "without %s",
                 static_cast<unsigned long long>(hash), size, what);
    dd_log(reshade::log::level::warning, text);
}

const shader_desc *pixel_of(const variant &v)
{
    return static_cast<const shader_desc *>(v.desc.subs[v.desc.pixel_shader].data);
}

// Under g_lock.
void retire(const std::shared_ptr<variant> &v, uint64_t handle)
{
    g_retired.push_back({v, handle, g_frame.load() + RETIRE_FRAMES});
}

// Under g_lock. Destroys every copy of a variant the game is done with.
void bury(const std::shared_ptr<variant> &v)
{
    v->dead = true;
    v->active = 0;
    v->ssr_active = 0;
    for (uint64_t handle : v->made)
        v->dev->destroy_pipeline({handle});
    v->made.clear();
    g_retired.erase(std::remove_if(g_retired.begin(), g_retired.end(),
                                   [&](const retired_copy &r) { return r.owner == v; }),
                    g_retired.end());
}

// The copy that reads our reflection and indirect light textures too (whichever is on). Under the shared
// work lock. A variant that takes neither keeps only its normal copy and the hook never swaps its table.
void build_ssr(const std::shared_ptr<variant> &v, light_values values, uint32_t reg, uint32_t ssil, uint32_t gen)
{
    if (v->ssr_failed)
        return;
    values.ssr2 = reg;
    values.ssil = ssil;
    {
        std::lock_guard lock(g_lock);
        values.ssr2_gloss = g_ssr2_gloss;
    }
    const shader_desc *ps = pixel_of(*v);
    std::vector<uint8_t> code;
    if (!light_patch(static_cast<const uint8_t *>(ps->code), ps->code_size, values, code))
    {
        v->ssr_failed = true;
        const light_support s = light_check(static_cast<const uint8_t *>(ps->code), ps->code_size);
        std::lock_guard lock(g_lock);
        const char *why = reg != 0 ? s.ssr2_missing : s.ssil_missing;
        const std::string what =
            std::string(reg != 0 ? "surface reflections (" : "indirect lighting (") + (why != nullptr ? why : "patch failed") + ")";
        report(v->hash, ps->code_size, what.c_str(), "", false);
        return;
    }
    std::vector<pipeline_subobject> subs = v->desc.subs;
    shader_desc patched = *ps;
    patched.code = code.data();
    patched.code_size = code.size();
    subs[v->desc.pixel_shader].data = &patched;
    pipeline copy = {0};
    t_creating = true;
    const bool made = v->dev->create_pipeline(v->layout, uint32_t(subs.size()), subs.data(), &copy);
    t_creating = false;
    std::lock_guard lock(g_lock);
    if (!made)
    {
        v->ssr_failed = true;
        report(v->hash, ps->code_size, "surface reflections (D3D12 rejected the copy)", "", false);
        return;
    }
    ++g_copies;
    if (v->dead || gen != g_gen.load())
    {
        v->dev->destroy_pipeline(copy);
        return;
    }
    v->made.push_back(copy.handle);
    if (const uint64_t old = v->ssr_active.exchange(copy.handle); old != 0)
        retire(v, old);
}

void build(const std::shared_ptr<variant> &v)
{
    std::shared_lock work(g_work_lock);
    light_values values;
    uint32_t gen;
    uint32_t ssr2 = 0, ssil = 0;
    {
        std::lock_guard lock(g_lock);
        v->queued = false;
        if (v->dead || v->failed)
            return;
        values = g_values;
        gen = g_gen.load();
        ssr2 = g_ssr2;
        ssil = g_ssil;
    }
    if (v->ssr_only)
        values = light_values{}; // the game's AO values: no AO copy, only ours
    if (ssr2 != 0 || ssil != 0)
        build_ssr(v, values, ssr2, ssil, gen);
    {
        std::lock_guard again(g_lock);
        if (!light_changes(values))
        {
            v->built = gen;
            return;
        }
    }
    const shader_desc *ps = pixel_of(*v);
    std::vector<uint8_t> code;
    if (!light_patch(static_cast<const uint8_t *>(ps->code), ps->code_size, values, code))
    {
        std::lock_guard lock(g_lock);
        v->failed = true;
        report(v->hash, ps->code_size, "", "the patch could not be made", true);
        return;
    }
    std::vector<pipeline_subobject> subs = v->desc.subs;
    shader_desc patched = *ps;
    patched.code = code.data();
    patched.code_size = code.size();
    subs[v->desc.pixel_shader].data = &patched;
    pipeline copy = {0};
    t_creating = true;
    const bool made = v->dev->create_pipeline(v->layout, uint32_t(subs.size()), subs.data(), &copy);
    t_creating = false;

    std::lock_guard lock(g_lock);
    if (!made)
    {
        v->failed = true;
        g_refused.insert(v->hash);
        report(v->hash, ps->code_size, "", "D3D12 rejected the copy", true);
        return;
    }
    ++g_copies;
    // Not wanted any more. It was never bound and can go at once.
    if (v->dead || gen != g_gen.load())
    {
        v->dev->destroy_pipeline(copy);
        return;
    }
    v->made.push_back(copy.handle);
    // In one step, so a bind on another thread gets either the old pipeline or the new one.
    const uint64_t old = v->active.exchange(copy.handle);
    v->built = gen;
    if (old != 0)
        retire(v, old);
}

DWORD WINAPI worker(void *module)
{
    for (;;)
    {
        std::shared_ptr<variant> v;
        {
            std::unique_lock lock(g_queue_lock);
            if (g_queue.empty() && !g_stop)
                g_queue_wake.wait_for(lock, std::chrono::seconds(2), [] { return !g_queue.empty() || g_stop; });
            if (g_queue.empty() || g_stop)
            {
                g_queue.clear();
                --g_workers;
                break;
            }
            v = std::move(g_queue.front());
            g_queue.pop_front();
        }
        build(v);
    }
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

// From the bind event: the variant needs a copy for the current values.
void request(const std::shared_ptr<variant> &v)
{
    std::lock_guard lock(g_queue_lock);
    if (g_stop)
    {
        v->queued = false;
        return;
    }
    g_queue.push_back(v);
    if (g_workers >= MAX_WORKERS || size_t(g_workers) >= g_queue.size())
    {
        g_queue_wake.notify_one();
        return;
    }
    // The worker keeps the add-on loaded until it ends.
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&worker), &module))
    {
        g_queue.pop_back();
        v->queued = false;
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, worker, module, 0, nullptr);
    if (thread == nullptr)
    {
        FreeLibrary(module);
        g_queue.pop_back();
        v->queued = false;
        return;
    }
    CloseHandle(thread);
    ++g_workers;
}

void on_init_pipeline(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects,
                      pipeline pipe)
{
    if (t_creating || pipe.handle == 0 || dev->get_api() != device_api::d3d12)
        return;
    const shader_desc *vs = nullptr, *ps = nullptr;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (subobjects[i].data == nullptr || subobjects[i].count == 0)
            continue;
        if (subobjects[i].type == pipeline_subobject_type::vertex_shader)
            vs = static_cast<const shader_desc *>(subobjects[i].data);
        else if (subobjects[i].type == pipeline_subobject_type::pixel_shader)
            ps = static_cast<const shader_desc *>(subobjects[i].data);
    }
    if (ps == nullptr || ps->code == nullptr ||
        !light_is_candidate(static_cast<const uint8_t *>(ps->code), ps->code_size))
        return;
    uint64_t hash = FNV_BASIS;
    if (vs != nullptr && vs->code != nullptr)
        hash = fnv(hash, vs->code, vs->code_size);
    hash = fnv(hash, ps->code, ps->code_size);

    const light_support s = light_check(static_cast<const uint8_t *>(ps->code), ps->code_size);
    auto v = std::make_shared<variant>();
    // A variant the AO edits can't take still gets the copy with our reflection or indirect light.
    const bool ssr_only = s.failed != nullptr && (s.ssr2_missing == nullptr || s.ssil_missing == nullptr);
    const bool copied = (s.failed == nullptr || ssr_only) && v->desc.copy(count, subobjects) &&
                        v->desc.pixel_shader != SIZE_MAX;
    {
        std::lock_guard lock(g_lock);
        if (g_patched == 0 && g_failures.empty())
            g_first_seen = std::chrono::steady_clock::now();
        if ((s.failed != nullptr && !ssr_only) || !copied)
        {
            report(hash, ps->code_size, "", s.failed != nullptr ? s.failed : "pipeline description not copied", true);
            return;
        }
        if (g_refused.count(hash) != 0)
            return;
        // Options this variant can't take, in one line.
        std::string missing;
        auto lacks = [&](const char *what, const char *why) {
            ++g_partial[what];
            missing += (missing.empty() ? "" : ", ") + std::string(what) + " (" + why + ")";
        };
        // Variants with no sun light have nothing for sunlight occlusion to do: counted, not logged.
        if (s.sun_path == 0 && s.sun_light)
            lacks("sunlight occlusion", s.sun_missing);
        else if (s.sun_path == 0)
            ++g_partial["no sun light (nothing to patch)"];
        if (!s.bounce)
            lacks("bounce light", "bounce site not found");
        v->ssr2_missing = s.ssr2_missing;
        v->ssil_missing = s.ssil_missing;
        if (ssr_only)
        {
            report(hash, ps->code_size,
                   (std::string("the AO lighting edits (") + s.failed + "), reflections and indirect light only").c_str(), "",
                   false);
            v->ssr_only = true;
        }
        else if (!missing.empty())
            report(hash, ps->code_size, missing.c_str(), "", false);
        if (g_patched++ == 0)
            dd_log(reshade::log::level::info, "Lighting: found the game's scene lighting shaders");
    }
    v->game = pipe.handle;
    v->hash = hash;
    v->dev = dev;
    v->layout = layout;
    std::unique_lock lock(g_map_lock);
    g_map[pipe.handle] = std::move(v);
    g_variants = g_map.size();
}

void on_destroy_pipeline(device *, pipeline pipe)
{
    if (pipe.handle == 0 || g_variants.load(std::memory_order_relaxed) == 0)
        return;
    {
        std::shared_lock lock(g_map_lock);
        if (g_map.find(pipe.handle) == g_map.end())
            return;
    }
    std::shared_ptr<variant> v;
    {
        std::unique_lock lock(g_map_lock);
        const auto it = g_map.find(pipe.handle);
        if (it == g_map.end())
            return;
        v = std::move(it->second);
        g_map.erase(it);
        g_variants = g_map.size();
    }
    // Lists the game recorded with the copies have run by the time it destroys its own pipeline.
    std::lock_guard lock(g_lock);
    bury(v);
}

void on_destroy_device(device *dev)
{
    std::vector<std::shared_ptr<variant>> gone;
    {
        std::unique_lock lock(g_map_lock);
        for (auto it = g_map.begin(); it != g_map.end();)
            if (it->second->dev == dev)
            {
                gone.push_back(std::move(it->second));
                it = g_map.erase(it);
            }
            else
                ++it;
        g_variants = g_map.size();
    }
    {
        std::lock_guard lock(g_lock);
        for (const auto &v : gone)
            bury(v);
    }
    {
        std::lock_guard lock(g_queue_lock);
        g_queue.erase(std::remove_if(g_queue.begin(), g_queue.end(),
                                     [&](const std::shared_ptr<variant> &v) { return v->dev == dev; }),
                      g_queue.end());
    }
    // A copy made right now on this device finishes and is destroyed before the device goes.
    std::unique_lock work(g_work_lock);
}

void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipe)
{
    if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::pixel_shader)) == 0 ||
        !g_need.load(std::memory_order_acquire) || g_variants.load(std::memory_order_relaxed) == 0)
        return;
    std::shared_lock lock(g_map_lock);
    const auto it = g_map.find(pipe.handle);
    if (it == g_map.end())
        return;
    variant &v = *it->second;
    if (v.failed.load(std::memory_order_relaxed))
        return;
    if (v.built.load(std::memory_order_acquire) != g_gen.load(std::memory_order_acquire) && !v.queued.exchange(true))
        request(it->second);
    // Set on the D3D12 list directly. ReShade's bind_pipeline also sets the primitive topology from the
    // pipeline's description. The game draws its lighting tiles as strips. Each quad lost a triangle.
    if (const uint64_t active = v.active.load(std::memory_order_acquire); active != 0)
        if (auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native()); native != nullptr)
            native->SetPipelineState(reinterpret_cast<ID3D12PipelineState *>(active));
}

void log_summary()
{
    std::string text = "Lighting: " + std::to_string(g_patched) + " variants can be patched, " +
                       std::to_string(g_copies) + " copies made so far";
    int alone = 0;
    for (const auto &f : g_failures)
        alone += f.second;
    text += ", " + std::to_string(alone) + " left alone";
    for (const auto &f : g_failures)
        text += (&f == &*g_failures.begin() ? " (" : ", ") + f.first + " " + std::to_string(f.second);
    if (!g_failures.empty())
        text += ")";
    for (const auto &f : g_partial)
        text += ", without " + f.first + " " + std::to_string(f.second);
    dd_log(reshade::log::level::info, text.c_str());
}

// Once a frame: destroys replaced copies once no command list can still use them.
void on_present(effect_runtime *)
{
    const uint64_t frame = ++g_frame;
    std::lock_guard lock(g_lock);
    for (size_t i = 0; i < g_retired.size();)
    {
        const retired_copy &r = g_retired[i];
        if (r.frame > frame)
        {
            ++i;
            continue;
        }
        r.owner->dev->destroy_pipeline({r.handle});
        auto &made = r.owner->made;
        made.erase(std::remove(made.begin(), made.end(), r.handle), made.end());
        g_retired.erase(g_retired.begin() + i);
    }
    if (!g_summary_done && (g_patched != 0 || !g_failures.empty()) &&
        std::chrono::steady_clock::now() - g_first_seen > std::chrono::minutes(1))
    {
        g_summary_done = true;
        log_summary();
    }
}
} // namespace

void light_register()
{
    {
        std::lock_guard lock(g_queue_lock);
        g_stop = false;
    }
    reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::register_event<reshade::addon_event::reshade_present>(on_present);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
}

void light_unregister()
{
    reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::unregister_event<reshade::addon_event::reshade_present>(on_present);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
    std::lock_guard lock(g_queue_lock);
    g_stop = true;
    g_queue.clear();
    g_queue_wake.notify_all();
}

void light_set(const float values[AO_PARAMS])
{
    const light_values v = from_ao(values);
    std::lock_guard lock(g_lock);
    if (g_values_set && same(v, g_values))
        return;
    g_values_set = true;
    g_values = v;
    g_need = light_changes(v);
    g_need = g_need || g_ssr2 != 0 || g_ssil != 0;
    ++g_gen;
    if (g_need)
        return;
    // Back to the game's own at once.
    std::shared_lock map(g_map_lock);
    for (const auto &entry : g_map)
        if (const uint64_t old = entry.second->active.exchange(0); old != 0)
            retire(entry.second, old);
}

void light_set_ssr2(uint32_t reg, bool gloss)
{
    std::lock_guard lock(g_lock);
    if (g_ssr2 == reg && g_ssr2_gloss == gloss)
        return;
    g_ssr2 = reg;
    g_ssr2_gloss = gloss;
    ++g_gen;
    g_need = light_changes(g_values) || reg != 0 || g_ssil != 0;
    std::shared_lock map(g_map_lock);
    for (const auto &entry : g_map)
    {
        // Another set of edits: a copy that could not be made before may work now.
        entry.second->ssr_failed = false;
        if (reg == 0 && g_ssil == 0)
            if (const uint64_t old = entry.second->ssr_active.exchange(0); old != 0)
                retire(entry.second, old);
        if (!g_need)
            if (const uint64_t old = entry.second->active.exchange(0); old != 0)
                retire(entry.second, old);
    }
}

void light_set_ssr2_gloss(bool gloss)
{
    std::lock_guard lock(g_lock);
    if (g_ssr2_gloss == gloss)
        return;
    g_ssr2_gloss = gloss;
    if (g_ssr2 != 0)
        ++g_gen; // only the copies with our reflection export it
}

void light_set_ssil(uint32_t reg)
{
    std::lock_guard lock(g_lock);
    if (g_ssil == reg)
        return;
    g_ssil = reg;
    ++g_gen;
    g_need = light_changes(g_values) || g_ssr2 != 0 || reg != 0;
    std::shared_lock map(g_map_lock);
    for (const auto &entry : g_map)
    {
        entry.second->ssr_failed = false;
        if (reg == 0 && g_ssr2 == 0)
            if (const uint64_t old = entry.second->ssr_active.exchange(0); old != 0)
                retire(entry.second, old);
        if (!g_need)
            if (const uint64_t old = entry.second->active.exchange(0); old != 0)
                retire(entry.second, old);
    }
}

bool light_ssr_pipelines(uint64_t pipe, uint64_t *ssr, uint64_t *fallback, uint64_t *hash, const char **missing)
{
    if (g_variants.load(std::memory_order_relaxed) == 0)
        return false;
    std::shared_lock lock(g_map_lock);
    const auto it = g_map.find(pipe);
    if (it == g_map.end())
        return false;
    const variant &v = *it->second;
    *ssr = v.ssr_active.load(std::memory_order_acquire);
    const uint64_t active = v.active.load(std::memory_order_acquire);
    *fallback = active != 0 ? active : pipe;
    *hash = v.hash;
    *missing = v.ssr2_missing != nullptr ? v.ssr2_missing : v.ssr_failed ? "D3D12 rejected the copy" : nullptr;
    return true;
}
