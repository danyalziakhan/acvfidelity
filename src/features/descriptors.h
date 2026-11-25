#pragma once

#include <reshade.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// What the game wrote into each descriptor heap slot, so a bound table can be
// read back: the view of an SRV or UAV, the buffer range of a CBV. The game
// writes and copies thousands of descriptors a frame on several threads. So a
// slot is found by shift and mask in chunks that never move, without a lock.
// A run of slots in one chunk is copied at once. For the two questions asked
// of it, it answers the same as ReShade's descriptor_tracking example.
// Samplers and pipeline layouts are not kept.
class __declspec(uuid("2d8f4b17-93c6-4e0a-b5f1-7a6c2e9d4b30")) descriptor_tracker
{
public:
    static void register_events();
    static void unregister_events();

    // The device's tracker. The game has one device. So it is kept rather than
    // looked up through ReShade on every call.
    static descriptor_tracker *of(reshade::api::device *device);

    // The view in a slot, or 0 for an empty slot, a slot without one, or a
    // heap never written to.
    reshade::api::resource_view view(reshade::api::descriptor_heap heap, uint32_t offset) const;
    // The buffer range of a constant buffer or storage buffer slot, or empty.
    reshade::api::buffer_range buffer(reshade::api::descriptor_heap heap, uint32_t offset) const;

    // Where the descriptor in a slot was written: the heap slot the game created it in (a copy keeps the source's
    // origin). False for an empty slot, or when the origin slot was written again since (the descriptor can't be
    // copied from there any more).
    bool origin(reshade::api::descriptor_heap heap, uint32_t offset, uint64_t &origin_heap, uint32_t &origin_offset,
                reshade::api::descriptor_type &type) const;

    descriptor_tracker();
    ~descriptor_tracker();

private:
    struct entry
    {
        uint64_t a = 0, b = 0, c = 0; // view handle, or buffer handle, offset and size
        uint64_t origin_heap = 0;     // where the game created it
        uint32_t origin_offset = 0;
        uint32_t serial = 0;          // of the write that created it; a copy keeps it
        reshade::api::descriptor_type type = reshade::api::descriptor_type::sampler;
        bool used = false;
    };

    static const uint32_t CHUNK_BITS = 12;
    static const uint32_t CHUNK = 1u << CHUNK_BITS;
    // D3D12 allows a million descriptors in a shader-visible heap. This covers
    // twice that.
    static const uint32_t MAX_CHUNKS = 512;

    struct heap_data
    {
        std::atomic<entry *> chunks[MAX_CHUNKS] = {};
        ~heap_data();
        const entry *find(uint32_t offset) const;
        entry *slot(uint32_t offset); // allocates the chunk if needed
    };

    const heap_data *find_heap(uint64_t handle) const;
    heap_data *heap(uint64_t handle);

    // Tells trackers apart in the per-thread cache, where an address could be
    // a later tracker's.
    uint64_t _id;
    mutable std::mutex _lock; // only for adding a heap
    std::unordered_map<uint64_t, heap_data *> _heaps;

    static bool on_copy(reshade::api::device *device, uint32_t count, const reshade::api::descriptor_table_copy *copies);
    static bool on_update(reshade::api::device *device, uint32_t count,
                          const reshade::api::descriptor_table_update *updates);
};
