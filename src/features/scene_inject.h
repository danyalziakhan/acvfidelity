#pragma once

// The scene hook. Lets our own compute passes run inside the game's frame and feeds their results to the scene
// lighting. The surface reflections (ssr2.cpp) and the indirect lighting (ssil.cpp) use it.
//
// What it does:
// - Has its own shader-visible CBV/SRV/UAV heap. The game's is at the device's limit (1000000 descriptors) and
//   the game writes all of it as a ring, so there is no room there. Our passes and every scene-lighting draw that
//   reads our textures run with our heap set (beside the game's sampler heap). For such a draw every view table of
//   the lighting is copied into our heap (once a frame per game table). Before any other GPU work on that list
//   (a draw, dispatch, table bind, UAV clear, close) the game's heaps and all its tables are set back.
// - Right before the first scene-lighting draw of a frame (the G-buffer is final there) it runs the shared
//   passes, then every registered pass, on the game's command list. Our root signature and pipelines are set
//   with native calls. The game's compute root state and pipeline are set back afterwards.
// - Shared passes: the camera block (current, inverse, previous view-projection, camera positions, exposure and
//   last frame's exposure, cut detection), linear depth at full and half resolution (a plain mip 1), decoded
//   normals with roughness, the forward-depth pyramid, and the lit colour capture.
// - The lit colour capture: when the lighting's render target is replaced on that list (or the list is closed)
//   after scene-lighting draws, the target is copied to an RGBA16F texture with a mip chain, in exposed units.
// - At every scene-lighting draw with a patched copy ready: the copied space-2 table holds our two textures at
//   t14 (slot 0) and t15 (slot 1, a null view until a pass sets it), and the copy of the lighting pipeline that
//   reads them is bound. Anything not found: the game's own (or the AO copy) stays bound with the game's heaps,
//   and the reason is logged once.

#include <d3d12.h>

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace scene
{
// Our textures. All live in one canonical state between batches (see dispatch).
struct texture
{
    ID3D12Resource *res = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0, mips = 1;
    bool output = false; // read by the lighting's pixel shaders too
    bool uav_home = false; // written by the lighting's pixel shaders (its canonical state is unordered access)
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
};

// The game's inputs a pass can bind.
enum input : uint32_t
{
    INPUT_DEPTH,    // s1 t10
    INPUT_NORMAL,   // s1 t7
    INPUT_MATERIAL, // s1 t8
    INPUT_ALBEDO,   // s1 t6
    INPUT_EXPOSURE, // s1 t0, structured, stride 4
    INPUT_MOTION,   // the game's packed motion vectors, found at its AO history pass (space-4 t4 there)
    INPUT_COUNT
};

// One slot of a pass's SRV table (t0..t6; t7 is always the camera block) or UAV table (u0..u7).
struct view
{
    enum kind_t : uint8_t
    {
        none,
        ours,
        game,
        camera
    } kind = none;
    texture *tex = nullptr;
    int mip = -1; // SRV: -1 every mip. UAV: the mip written (or read and written).
    input in = INPUT_DEPTH;

    static view of(texture &t, int mip = -1)
    {
        view v;
        v.kind = ours;
        v.tex = &t;
        v.mip = mip;
        return v;
    }
    static view from(input i)
    {
        view v;
        v.kind = game;
        v.in = i;
        return v;
    }
    static view cam()
    {
        view v;
        v.kind = camera;
        return v;
    }
};

// The frame as the passes see it.
struct frame
{
    ID3D12GraphicsCommandList *list = nullptr;
    uint64_t run = 0;           // how many frames the passes ran so far (history parity)
    uint32_t width = 0, height = 0; // G-buffer
    uint32_t flags = 0;         // FLAG_* of shaders/common.hlsli
    uint32_t pyramid_mips = 1;
};

// Shared textures, valid inside a pass.
struct shared_set
{
    texture *depth_now, *depth_before; // linear depth (m), mip 0 full, mip 1 half (plain)
    texture *normals;                  // world normal, w roughness, -1 sky
    texture *pyramid;                  // forward depth, nearest per texel, every mip
    texture *capture;                  // last frame's lit colour, exposed units, mips
    texture *motion;                   // uv offset to the previous frame (previous = uv - offset): the game's motion
                                       // vectors where found, else the camera's
    texture *lighting_export;          // RGBA16F, written by the patched lighting last frame at space-2 u0 (the
                                       // reflections' gloss export); bind it as a UAV (its home state)
};


// The users of the hook: each switches it on and off for itself. The hook runs while any user has it on.
const uint32_t USER_SSR2 = 0, USER_SSIL = 1, USERS = 2;

// Registered once by a user of the hook. Called once a frame after the shared passes, on the game's list.
typedef void (*pass_fn)(const frame &f, const shared_set &s);
void add_pass(pass_fn fn);
// Slot 0 is space-2 t14 of the lighting, slot 1 t15. nullptr binds a null view there.
void set_output(uint32_t slot, texture *t);

// (Re)creates a texture when its size or format changes. True when it was made new (no history in it).
bool make_texture(texture &t, DXGI_FORMAT format, uint32_t width, uint32_t height, uint32_t mips, bool output);
// Our pipeline for embedded DXBC (cached by address).
ID3D12PipelineState *pipeline(const void *code, size_t size);
// Records one dispatch: transitions, a fresh pair of tables in our heap, root constants (16 values), the game's
// view constants on b1. Our textures go back to their canonical state when the batch ends.
void dispatch(const frame &f, ID3D12PipelineState *pso, std::initializer_list<view> srvs,
              std::initializer_list<view> uavs, const uint32_t constants[16], uint32_t groups_x, uint32_t groups_y);
// The shared temporal accumulation (scene.hlsl CSTemporal) at a pass size that matches linear depth mip
// `depth_mip`: signal + history -> new history and history length.
void temporal(const frame &f, const shared_set &s, texture &signal, texture &history_before, texture &history_now,
              texture &length_before, texture &length_now, uint32_t depth_mip, float min_blend, float max_length);

// The add-on side.
void register_events(); // at load: the heap hook needs init_device
void unregister_events();
void present(); // once a frame
void enable(bool on);   // off by default; = enable_for(USER_SSR2, on)
void enable_for(uint32_t user, bool on);
bool enabled();
} // namespace scene
