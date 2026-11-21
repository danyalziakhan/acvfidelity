// The SSR pixel shader is recognized by its size and checksum. Only that exact
// build is ever changed. Some settings are literals already. The rest are
// constant buffer reads that become literals. No-ops pad them to keep the
// code size. Every place is checked against the game's code first. If one
// differs, the game's own pipeline stays.
//
// The game's pipeline is a graphics one. Changing a setting means making a
// new pipeline from its whole description. That description is copied when
// the game creates it. Copies live until the game destroys its own pipeline
// because recorded command lists may still use an older one.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include <reshade.hpp>

#include "log.h"
#include "pipeline_copy.h"
#include "shader_patch.h"
#include "ssr.h"
#include "ssr_angle.h"
#include "ssr_upscale_data.h"
#include "diagnostics.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

using namespace reshade::api;

namespace
{
const size_t SHADER_SIZE = 6924;
const uint8_t SHADER_CHECKSUM[16] = {0xe8, 0x76, 0x2b, 0x4b, 0xb9, 0xaf, 0x25, 0x06,
                                     0xa8, 0x93, 0xa8, 0x4c, 0x36, 0x05, 0x22, 0x52};

const uint32_t NOP = 0x100003a;

// An instruction whose last operand is a constant buffer value (4 tokens).
// The copy puts a 2-token immediate there and two no-ops after it. The code
// keeps its size.
struct site
{
    size_t at;
    uint32_t count; // tokens
    uint32_t game[9];
};
// max_sat r3.x, r3.x, CB[14].w: the least the normal is pulled toward up.
const site SMOOTHING = {0x5c8, 9, {0x9002034, 0x100012, 3, 0x10000a, 3, 0x30803a, 2, 0, 0xe}};
// mul r1.x, r1.x, CB[13].x: hit tolerance in steps' depths.
const site THICKNESS = {0xbe0, 9, {0x9000038, 0x100012, 1, 0x10000a, 1, 0x30800a, 2, 0, 0xd}};
// Ray length: the scale of the ray. Steps: the sample count (an integer) in
// the three places that read it.
const site LENGTH = {0xaac, 9, {0x9000038, 0x100072, 2, 0x100246, 2, 0x308ff6, 2, 0, 0xc}};
const site COUNT_ITOF = {0xba4, 7, {0x700002b, 0x100082, 0, 0x30802a, 2, 0, 0xc}};
const site COUNT_UTOF = {0xd08, 7, {0x7000056, 0x100022, 0, 0x30802a, 2, 0, 0xc}};
const site COUNT_LOOP = {0xe0c, 9, {0x9000050, 0x100042, 4, 0x10002a, 3, 0x30802a, 2, 0, 0xc}};
// mul_sat r3.y, r0.w, CB[15].x: the pixel's world size times this is how
// much the normal flattens with distance. More is flatter.
const site DISTANCE_SMOOTHING = {0x528, 9, {0x9002038, 0x100022, 3, 0x10003a, 0, 0x30800a, 2, 0, 0xf}};
// add r0.w, r0.w, CB[16].w: where the top and bottom fade starts (0.70).
const site EDGE_Y = {0x160c, 9, {0x9000000, 0x100082, 0, 0x10003a, 0, 0x30803a, 2, 0, 0x10}};
// mul r2.xyzw, r1.xyzw, CB[13].zzzz: hits on the sky (depth 0) times this.
const site SKY = {0x1948, 9, {0x9000038, 0x1000f2, 2, 0x100e46, 1, 0x308aa6, 2, 0, 0xd}};
const site *const SITES[] = {&SMOOTHING, &THICKNESS, &LENGTH, &COUNT_ITOF, &COUNT_UTOF, &COUNT_LOOP,
                             &DISTANCE_SMOOTHING, &EDGE_Y, &SKY};

// Literals already in the code: the token's offset and the game's bits.
struct literal
{
    size_t at;
    uint32_t game;
};
// mul r3.x, r3.x, l(0.01): distance in meters scales the exponent of the
// flattening, f^(d * this) + f with f < 1. More is less flattening far away.
const literal FAR_DETAIL = {0x54c + 24, 0x3c23d70a};
// lt r0.x, l(0.866020), r0.x: rays more toward the camera than this get none.
const literal RAY_CUTOFF = {0x7a0 + 16, 0x3f5db37d};
// mul r0.z, r0.z, l(0.7) in a log/exp: min(ray, 1)^this scales the tolerance
// of short rays. A smaller power is thicker.
const literal SHORT_THICKNESS = {0xb14 + 24, 0x3f333333};
// mul ..., l(0.5, -0.5, 0, 0.0175) in a log/exp: 1 - depth^this is the least
// tolerance. A larger power is thicker far away.
const literal FAR_THICKNESS = {0xc1c + 36, 0x3c8f5c29};
// mul r0.xy, r0.xyxx, l(1.5, 0.1): the per-pixel step jitter in steps and
// the steps it takes to ramp in (scaled by the noise).
const literal JITTER = {0xd4c + 24, 0x3fc00000};
const literal JITTER_RAMP = {0xd4c + 28, 0x3dcccccd};
// mul r0.w, r0.w, l(0.0125): the first sample is at least this far (times
// 128 / steps). This avoids self hits.
const literal START_OFFSET = {0xdb8 + 24, 0x3c4ccccd};
const literal *const LITERALS[] = {&FAR_DETAIL, &RAY_CUTOFF, &SHORT_THICKNESS, &FAR_THICKNESS,
                                   &JITTER, &JITTER_RAMP, &START_OFFSET};

// Whole instructions replaced. LIT marks the tokens the values go into.
struct rewrite
{
    size_t at;
    uint32_t count;
    uint32_t game[12];
    uint32_t replaced_count;
    uint32_t replaced[9];
    uint32_t slots;   // how many values go in
    uint32_t slot[3]; // the token indexes they go into
};
const uint32_t LIT = 0;
// The side fade is 1 - sat((|x| - start) * sharpness). It shares a rewrite
// with the top and bottom sharpness. mov r2.x, -CB[16].z becomes
// mov r2.xzw, l(-start, 0, side sharpness, top sharpness). Then
// mul_sat r1.zw, r1.zzzw, CB[16].xxxy reads r2.zzzw instead. r2.z and r2.w
// are free there.
const rewrite EDGE_X = {0x1630, 8, {0x8000036, 0x100012, 2, 0x8030802a, 0x41, 2, 0, 0x10},
                        8, {0x8000036, 0x1000d2, 2, 0x4002, LIT, 0, LIT, LIT}, 3, {4, 6, 7}};
const rewrite EDGE_SHARPNESS = {0x1688, 9, {0x9002038, 0x1000c2, 1, 0x100ea6, 1, 0x308406, 2, 0, 0x10},
                                7, {0x7002038, 0x1000c2, 1, 0x100ea6, 1, 0x100ea6, 2}, 0, {}};
// The output is lerp(CB[13].w, CB[14].x, water) * reflection. Both constants
// are 1 in the game. add r0.y, -CB[13].w, CB[14].x becomes mov r0.y, l(0).
// Then mad r0.x, r0.x, r0.y, CB[13].w adds l(intensity) instead.
const rewrite INTENSITY_A = {0x1a7c, 12, {0xc000000, 0x100022, 0, 0x8030803a, 0x41, 2, 0, 0xd, 0x30800a, 2, 0, 0xe},
                             5, {0x5000036, 0x100022, 0, 0x4001, 0}, 0, {}};
const rewrite INTENSITY_B = {0x1aac, 11, {0xb000032, 0x100012, 0, 0x10000a, 0, 0x10001a, 0, 0x30803a, 2, 0, 0xd},
                             9, {0x9000032, 0x100012, 0, 0x10000a, 0, 0x10001a, 0, 0x4001, LIT}, 1, {8}};
const rewrite *const REWRITES[] = {&EDGE_X, &EDGE_SHARPNESS, &INTENSITY_A, &INTENSITY_B};

// The reflected color is read from the previous frame (T3). The game reads it at this frame's uv and moves
// the hit only for the edge fade. So reflections trail the camera. The game's reprojected hit runs a frame
// ahead (tested: panning right puts it to the left). Halfway between the two lines up with the objects:
// lag measured +4 px, 0 px with this. sample_l r1, r0.xy, T3 then reads r9.xy, filled after the reprojection.
const size_t AT_REPROJECTED = 0x153c; // div r1.xy, r2.xyxx, r0.wwww
const uint32_t REPROJECTED_GAME[7] = {0x700000e, 0x100032, 1, 0x100046, 2, 0x100ff6, 0};
const size_t AT_COLOR_READ = 0x178c; // sample_l r1.xyzw, r0.xyxx, T3[2].xyzw, S0[2], l(0)
const uint32_t COLOR_READ_GAME[5] = {0xd000048, 0x1000f2, 1, 0x100046, 0};
// mad r9.xy, r1.xyxx, l(0.5, -0.5), l(0.5, 0.5); add r9.xy, r9, -r0.xy; mad r9.xy, r9, l(share), r0.xy
// The noise lookup: uv / (pixel size x 64). The pass is half resolution. 64 then puts each pixel between two
// noise texels. 128 gives every pixel a texel of its own.
const size_t AT_NOISE = 0x81c + 8 * 4; // mul r0.xy, CB2[0][9].xyxx, l(64, 64, 0, 0)
// The march keeps the first step within the hit tolerance. Refinement halves the last step a few times,
// keeping the half where the ray crosses the surface. Before mad r0.xy, r2.xyxx, r0.xxxx, r1.yzyy.
const size_t AT_HIT = 0x130c;
const uint32_t HIT_GAME[9] = {0x9000032, 0x100032, 0, 0x100046, 2, 0x100006, 0, 0x100596, 1};
const uint32_t REFINE_START[93] = {0x8000000, 0x100012, 0x4, 0x10000A, 0x0, 0x8010000A, 0x41, 0x3, 0x7000034, 0x100012, 0x4, 0x10000A, 0x4, 0x4001, 0x0, 0x5000036, 0x100022, 0x4, 0x10000A, 0x0, 0x9000032, 0x100032, 0x6, 0x100046, 0x2, 0x100006, 0x4, 0x100596, 0x1, 0x9000000, 0x100032, 0x6, 0x100046, 0x6, 0x308AE6, 0x2, 0x0, 0x9, 0xD000048, 0x100042, 0x6, 0x100046, 0x6, 0x207C96, 0x4, 0x3, 0x206000, 0x1, 0x0, 0x4001, 0x0, 0x8000000, 0x100082, 0x6, 0x8010002A, 0xC1, 0x6, 0x4001, 0x3F800000, 0x9000032, 0x100012, 0x7, 0x10002A, 0x2, 0x10000A, 0x4, 0x10003A, 0x1, 0x8000000, 0x100012, 0x7, 0x10000A, 0x7, 0x8010003A, 0x41, 0x6, 0xA000032, 0x100012, 0x7, 0x8010002A, 0x41, 0x0, 0x10000A, 0x1, 0x10000A, 0x7, 0x7000031, 0x100042, 0x4, 0x4001, 0x0, 0x10000A, 0x7};
const uint32_t REFINE_ROUND[112] = {0x7000000, 0x100012, 0x5, 0x10000A, 0x4, 0x10001A, 0x4, 0x7000038, 0x100012, 0x5, 0x10000A, 0x5, 0x4001, 0x3F000000, 0x9000032, 0x100032, 0x6, 0x100046, 0x2, 0x100006, 0x5, 0x100596, 0x1, 0x9000000, 0x100032, 0x6, 0x100046, 0x6, 0x308AE6, 0x2, 0x0, 0x9, 0xD000048, 0x100042, 0x6, 0x100046, 0x6, 0x207C96, 0x4, 0x3, 0x206000, 0x1, 0x0, 0x4001, 0x0, 0x8000000, 0x100082, 0x6, 0x8010002A, 0xC1, 0x6, 0x4001, 0x3F800000, 0x9000032, 0x100012, 0x7, 0x10002A, 0x2, 0x10000A, 0x5, 0x10003A, 0x1, 0x8000000, 0x100012, 0x7, 0x10000A, 0x7, 0x8010003A, 0x41, 0x6, 0xA000032, 0x100012, 0x7, 0x8010002A, 0x41, 0x0, 0x10000A, 0x1, 0x10000A, 0x7, 0x7000031, 0x100022, 0x7, 0x4001, 0x0, 0x10000A, 0x7, 0x7000020, 0x100042, 0x7, 0x10001A, 0x7, 0x10002A, 0x4, 0x9000037, 0x100012, 0x4, 0x10002A, 0x7, 0x10000A, 0x5, 0x10000A, 0x4, 0x9000037, 0x100022, 0x4, 0x10002A, 0x7, 0x10001A, 0x4, 0x10000A, 0x5};
const uint32_t REFINE_END[5] = {0x5000036, 0x100012, 0x0, 0x10001A, 0x4};
const uint32_t REPROJECT[35] = {0xF000032, 0x100032, 0x9, 0x100046, 0x1, 0x4002, 0x3F000000, 0xBF000000, 0x0, 0x0, 0x4002, 0x3F000000, 0x3F000000, 0x0, 0x0, 0x8000000, 0x100032, 0x9, 0x100046, 0x9, 0x80100046, 0x41, 0x0, 0xC000032, 0x100032, 0x9, 0x100046, 0x9, 0x4002, 0x3F000000, 0x3F000000, 0x0, 0x0, 0x100046, 0x0};
const int REPROJECT_SHARE[2] = {29, 30};

// The SSR resolve (a pixel shader after the trace) blends the trace with its history. The reflection is traced
// at half resolution. So reflected edges show as 2-pixel steps. Softening blends each pixel with its 4
// neighbors. Measured steps: 1.31 -> 1.15 at 0.5 and 1.08 at 1.0. 1.00 is none. It also limits the history to
// the range of those 5 pixels against ghosting. Inserted after the history read, sample_l r2, r0.xy, T0.
const size_t RESOLVE_SIZE = 1564;
const uint8_t RESOLVE_CHECKSUM[16] = {0x2b, 0x5e, 0x55, 0x19, 0xc3, 0x90, 0x0b, 0x3d,
                                      0xe4, 0x1b, 0x61, 0xff, 0x98, 0x85, 0xfc, 0xb2};
const size_t AT_RESOLVE_TEMPS = 0x1c8; // dcl_temps 4
const size_t AT_RESOLVE_CURRENT = 0x40c; // sample_l r1.xyzw, v1.xyxx, T2[3]
const uint32_t RESOLVE_CURRENT_GAME[13] = {0xd000048, 0x1000f2, 1, 0x101046, 1, 0x207e46, 2, 3, 0x206000, 0, 0, 0x4001, 0};
const size_t AT_RESOLVE_HISTORY = 0x440; // sample_l r2.xyzw, r0.xyxx, T0[0]
const uint32_t RESOLVE_HISTORY_GAME[13] = {0xd000048, 0x1000f2, 2, 0x100046, 0, 0x207e46, 0, 0, 0x206000, 0, 0, 0x4001, 0};
const uint32_t RESOLVE_BLEND[255] = {0x500007A, 0x100032, 0x4, 0x101046, 0x1, 0x500007C, 0x1000C2, 0x4, 0x101406, 0x1, 0x5000036, 0x1000F2, 0x7, 0x100E46, 0x1, 0x5000036, 0x1000F2, 0x8, 0x100E46, 0x1, 0x5000036, 0x1000F2, 0x3, 0x100E46, 0x1, 0x5000036, 0x100032, 0x5, 0x101046, 0x1, 0x7000000, 0x100012, 0x5, 0x10000A, 0x5, 0x10000A, 0x4, 0xD000048, 0x1000F2, 0x6, 0x100046, 0x5, 0x207E46, 0x2, 0x3, 0x206000, 0x0, 0x0, 0x4001, 0x0, 0x7000033, 0x1000F2, 0x7, 0x100E46, 0x7, 0x100E46, 0x6, 0x7000034, 0x1000F2, 0x8, 0x100E46, 0x8, 0x100E46, 0x6, 0x7000000, 0x1000F2, 0x3, 0x100E46, 0x3, 0x100E46, 0x6, 0x5000036, 0x100032, 0x5, 0x101046, 0x1, 0x8000000, 0x100012, 0x5, 0x10000A, 0x5, 0x8010000A, 0x41, 0x4, 0xD000048, 0x1000F2, 0x6, 0x100046, 0x5, 0x207E46, 0x2, 0x3, 0x206000, 0x0, 0x0, 0x4001, 0x0, 0x7000033, 0x1000F2, 0x7, 0x100E46, 0x7, 0x100E46, 0x6, 0x7000034, 0x1000F2, 0x8, 0x100E46, 0x8, 0x100E46, 0x6, 0x7000000, 0x1000F2, 0x3, 0x100E46, 0x3, 0x100E46, 0x6, 0x5000036, 0x100032, 0x5, 0x101046, 0x1, 0x7000000, 0x100022, 0x5, 0x10001A, 0x5, 0x10003A, 0x4, 0xD000048, 0x1000F2, 0x6, 0x100046, 0x5, 0x207E46, 0x2, 0x3, 0x206000, 0x0, 0x0, 0x4001, 0x0, 0x7000033, 0x1000F2, 0x7, 0x100E46, 0x7, 0x100E46, 0x6, 0x7000034, 0x1000F2, 0x8, 0x100E46, 0x8, 0x100E46, 0x6, 0x7000000, 0x1000F2, 0x3, 0x100E46, 0x3, 0x100E46, 0x6, 0x5000036, 0x100032, 0x5, 0x101046, 0x1, 0x8000000, 0x100022, 0x5, 0x10001A, 0x5, 0x8010003A, 0x41, 0x4, 0xD000048, 0x1000F2, 0x6, 0x100046, 0x5, 0x207E46, 0x2, 0x3, 0x206000, 0x0, 0x0, 0x4001, 0x0, 0x7000033, 0x1000F2, 0x7, 0x100E46, 0x7, 0x100E46, 0x6, 0x7000034, 0x1000F2, 0x8, 0x100E46, 0x8, 0x100E46, 0x6, 0x7000000, 0x1000F2, 0x3, 0x100E46, 0x3, 0x100E46, 0x6, 0x7000034, 0x1000F2, 0x2, 0x100E46, 0x2, 0x100E46, 0x7, 0x7000033, 0x1000F2, 0x2, 0x100E46, 0x2, 0x100E46, 0x8, 0xA000038, 0x1000F2, 0x3, 0x100E46, 0x3, 0x4002, 0x3E4CCCCD, 0x3E4CCCCD, 0x3E4CCCCD, 0x3E4CCCCD, 0x8000000, 0x1000F2, 0x3, 0x100E46, 0x3, 0x80100E46, 0x41, 0x1, 0xC000032, 0x1000F2, 0x1, 0x100E46, 0x3, 0x4002, 0x3F000000, 0x3F000000, 0x3F000000, 0x3F000000, 0x100E46, 0x1};
const int RESOLVE_SOFT[4] = {249, 250, 251, 252};

// The current values, under g_lock.
float g_values[SSR_PARAMS];

// The bind event reads `game` and `active` without a lock. Everything else
// changes under g_lock.
struct slot
{
    std::atomic<uint64_t> game{0};
    std::atomic<uint64_t> active{0}; // 0: the game's own
    device *dev = nullptr;
    pipeline_layout layout = {0};
    stored_desc *desc = nullptr;
    std::vector<uint64_t> made;
    bool resolve = false; // the resolve pass, not the trace
    int water = -1;       // a water lighting shader (UPSCALE_SHADERS index)
};
const int SLOTS = 8;
slot g_slots[SLOTS];
std::mutex g_lock;
std::atomic<bool> g_ready{false};
std::atomic<int> g_logged{0};

uint32_t bits(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

bool is_game_shader(const shader_desc &shader)
{
    return shader.code != nullptr && shader.code_size == SHADER_SIZE &&
           memcmp(static_cast<const uint8_t *>(shader.code) + 4, SHADER_CHECKSUM, 16) == 0;
}

void patch(std::vector<uint8_t> &code, const site &where, uint32_t value)
{
    uint32_t tokens[9] = {};
    const uint32_t keep = where.count - 4; // everything before the CB operand
    memcpy(tokens, where.game, keep * 4);
    tokens[0] = (tokens[0] & ~0x7F000000u) | ((where.count - 2) << 24);
    tokens[keep] = 0x4001; // a 1-component immediate
    tokens[keep + 1] = value;
    tokens[keep + 2] = NOP;
    tokens[keep + 3] = NOP;
    memcpy(code.data() + where.at, tokens, where.count * 4);
}

// The values go into the rewrite's slots in order.
void patch(std::vector<uint8_t> &code, const rewrite &where, std::initializer_list<uint32_t> values)
{
    uint32_t tokens[12];
    for (uint32_t i = 0; i < where.count; ++i)
        tokens[i] = i < where.replaced_count ? where.replaced[i] : NOP;
    uint32_t n = 0;
    for (uint32_t value : values)
        if (n < where.slots)
            tokens[where.slot[n++]] = value;
    memcpy(code.data() + where.at, tokens, where.count * 4);
}

uint32_t read_u32(const std::vector<uint8_t> &code, size_t at)
{
    uint32_t v = 0;
    if (at + 4 <= code.size())
        memcpy(&v, code.data() + at, 4);
    return v;
}

void write_u32(std::vector<uint8_t> &code, size_t at, uint32_t v)
{
    if (at + 4 <= code.size())
        memcpy(code.data() + at, &v, 4);
}

bool differs(const float *v, int i)
{
    return v[i] != SSR_DEFS[i].game;
}

// Patches the game's SSR pixel shader for the values. False if any place differs from the known code or an
// insertion fails: the game's own pipeline then stays.
bool make_ssr_copy(std::vector<uint8_t> &code, const float *v)
{
    bool matches = true;
    for (const site *where : SITES)
        matches &= memcmp(code.data() + where->at, where->game, where->count * 4) == 0;
    for (const literal *where : LITERALS)
        matches &= memcmp(code.data() + where->at, &where->game, 4) == 0;
    for (const rewrite *where : REWRITES)
        matches &= memcmp(code.data() + where->at, where->game, where->count * 4) == 0;
    if (!matches)
        return false;
    if (differs(v, SSR_SMOOTHING))
        patch(code, SMOOTHING, bits(v[SSR_SMOOTHING]));
    if (differs(v, SSR_THICKNESS))
        patch(code, THICKNESS, bits(v[SSR_THICKNESS]));
    if (differs(v, SSR_LENGTH))
        patch(code, LENGTH, bits(v[SSR_LENGTH]));
    if (differs(v, SSR_STEPS))
    {
        // The loop takes 4 samples a pass. The count stays a multiple of 4.
        const uint32_t samples = std::max(uint32_t(v[SSR_STEPS]) / 4 * 4, 4u);
        patch(code, COUNT_ITOF, samples);
        patch(code, COUNT_UTOF, samples);
        patch(code, COUNT_LOOP, samples);
    }
    if (differs(v, SSR_DISTANCE_SMOOTHING))
        patch(code, DISTANCE_SMOOTHING, bits(v[SSR_DISTANCE_SMOOTHING]));
    if (differs(v, SSR_EDGE_Y))
        patch(code, EDGE_Y, bits(v[SSR_EDGE_Y]));
    if (differs(v, SSR_SKY))
        patch(code, SKY, bits(v[SSR_SKY]));
    auto set = [&](const literal &where, float value) { memcpy(code.data() + where.at, &value, 4); };
    if (differs(v, SSR_FAR_DETAIL))
        set(FAR_DETAIL, v[SSR_FAR_DETAIL]);
    if (differs(v, SSR_RAY_CUTOFF))
        set(RAY_CUTOFF, v[SSR_RAY_CUTOFF]);
    // The window shows the inverse of the power. That way higher is thicker.
    if (differs(v, SSR_SHORT_THICKNESS))
        set(SHORT_THICKNESS, 1.0f / v[SSR_SHORT_THICKNESS]);
    if (differs(v, SSR_FAR_THICKNESS))
        set(FAR_THICKNESS, v[SSR_FAR_THICKNESS]);
    if (differs(v, SSR_JITTER))
        set(JITTER, v[SSR_JITTER]);
    if (differs(v, SSR_JITTER_RAMP))
        set(JITTER_RAMP, v[SSR_JITTER_RAMP]);
    if (differs(v, SSR_START_OFFSET))
        set(START_OFFSET, v[SSR_START_OFFSET]);
    if (differs(v, SSR_EDGE_X) || differs(v, SSR_EDGE_X_SHARPNESS) || differs(v, SSR_EDGE_Y_SHARPNESS))
    {
        patch(code, EDGE_X, {bits(-v[SSR_EDGE_X]), bits(v[SSR_EDGE_X_SHARPNESS]), bits(v[SSR_EDGE_Y_SHARPNESS])});
        patch(code, EDGE_SHARPNESS, {});
    }
    if (differs(v, SSR_INTENSITY))
    {
        patch(code, INTENSITY_A, {});
        patch(code, INTENSITY_B, {bits(v[SSR_INTENSITY])});
    }
    // Insertions last, back to front: they move everything after them.
    const bool motion = differs(v, SSR_MOTION), refine = v[SSR_REFINE] >= 1.0f;
    if ((motion && (memcmp(code.data() + AT_REPROJECTED, REPROJECTED_GAME, sizeof(REPROJECTED_GAME)) != 0 ||
                    memcmp(code.data() + AT_COLOR_READ, COLOR_READ_GAME, sizeof(COLOR_READ_GAME)) != 0)) ||
        (refine && memcmp(code.data() + AT_HIT, HIT_GAME, sizeof(HIT_GAME)) != 0) ||
        (differs(v, SSR_NOISE) && read_u32(code, AT_NOISE) != bits(64.0f)))
        return false;
    if (differs(v, SSR_NOISE))
        for (int k = 0; k < 2; ++k)
            write_u32(code, AT_NOISE + 4 * k, bits(v[SSR_NOISE]));
    if (motion)
    {
        write_u32(code, AT_COLOR_READ + 4 * 4, 9); // the coordinate register: r0 to r9
        uint32_t tokens[std::size(REPROJECT)];
        memcpy(tokens, REPROJECT, sizeof(tokens));
        for (int i : REPROJECT_SHARE)
            tokens[i] = bits(v[SSR_MOTION]);
        if (!dxbc_insert(code, AT_REPROJECTED + sizeof(REPROJECTED_GAME), tokens, uint32_t(std::size(tokens))))
            return false;
    }
    if (refine)
    {
        std::vector<uint32_t> tokens(std::begin(REFINE_START), std::end(REFINE_START));
        for (int k = 0; k < int(v[SSR_REFINE]); ++k)
            tokens.insert(tokens.end(), std::begin(REFINE_ROUND), std::end(REFINE_ROUND));
        tokens.insert(tokens.end(), std::begin(REFINE_END), std::end(REFINE_END));
        if (!dxbc_insert(code, AT_HIT, tokens.data(), uint32_t(tokens.size())))
            return false;
    }
    uint32_t sum[4];
    dxbc_checksum(code.data(), code.size(), sum);
    memcpy(code.data() + 4, sum, 16);
    return true;
}

bool is_resolve_shader(const shader_desc &shader)
{
    return shader.code != nullptr && shader.code_size == RESOLVE_SIZE &&
           memcmp(static_cast<const uint8_t *>(shader.code) + 4, RESOLVE_CHECKSUM, 16) == 0;
}

// Patches the resolve for the softening. False if any place differs from the known code.
bool make_resolve_copy(std::vector<uint8_t> &code, const float *v)
{
    if (memcmp(code.data() + AT_RESOLVE_CURRENT, RESOLVE_CURRENT_GAME, sizeof(RESOLVE_CURRENT_GAME)) != 0 ||
        memcmp(code.data() + AT_RESOLVE_HISTORY, RESOLVE_HISTORY_GAME, sizeof(RESOLVE_HISTORY_GAME)) != 0 ||
        read_u32(code, AT_RESOLVE_TEMPS) != 0x2000068 || read_u32(code, AT_RESOLVE_TEMPS + 4) != 4)
        return false;
    write_u32(code, AT_RESOLVE_TEMPS + 4, 9);
    uint32_t tokens[std::size(RESOLVE_BLEND)];
    memcpy(tokens, RESOLVE_BLEND, sizeof(tokens));
    for (int i : RESOLVE_SOFT)
        tokens[i] = bits(v[SSR_SOFTEN]);
    if (!dxbc_insert(code, AT_RESOLVE_HISTORY + sizeof(RESOLVE_HISTORY_GAME), tokens, uint32_t(std::size(tokens))))
        return false;
    uint32_t sum[4];
    dxbc_checksum(code.data(), code.size(), sum);
    memcpy(code.data() + 4, sum, 16);
    return true;
}

// The water lighting reads the half-resolution reflection with one bilinear sample. Bilinear has a kink at
// every texel, so reflected edges show 2-pixel steps. The smooth upscale adds a B-spline read (4 bilinear taps)
// and mixes it in by the setting. Measured at Fidelity III with Softening 0.75: steps 1.07 -> 1.01, about 3%
// less local contrast. The SSR texel is 2 x the shader's own 1 / full-res size (CB2[0][52].xy).
int water_shader(const shader_desc &shader)
{
    if (shader.code == nullptr)
        return -1;
    for (int i = 0; i < int(std::size(UPSCALE_SHADERS)); ++i)
        if (shader.code_size == UPSCALE_SHADERS[i].size &&
            memcmp(static_cast<const uint8_t *>(shader.code) + 4, UPSCALE_SHADERS[i].checksum, 16) == 0)
            return i;
    return -1;
}

// False if the read or dcl_temps differ from the known code.
bool make_upscale_copy(std::vector<uint8_t> &code, const float *v, int which)
{
    const upscale_shader &u = UPSCALE_SHADERS[which];
    if (code.size() != u.size || u.at + sizeof(u.site) > code.size() ||
        memcmp(code.data() + u.at, u.site, sizeof(u.site)) != 0)
        return false;
    const upscale_filter &f = u.filter[v[SSR_UPSCALE_FILTER] >= 1.5f ? 1 : 0];
    std::vector<uint32_t> post(f.post, f.post + f.post_count);
    for (int i : f.k)
        post[i] = bits(v[SSR_UPSCALE]);
    if (!dxbc_set_temps(code, u.temps + f.temps) ||
        !dxbc_insert(code, u.at + sizeof(u.site), post.data(), post.size()) ||
        !dxbc_insert(code, u.at, f.pre, f.pre_count))
        return false;
    uint32_t sum[4];
    dxbc_checksum(code.data(), code.size(), sum);
    memcpy(code.data() + 4, sum, 16);
    return true;
}

// Under g_lock.
void apply_to(slot &s)
{
    const float *v = g_values;
    bool any = false;
    for (int i = 0; i < SSR_PARAMS; ++i)
        any |= s.water >= 0  ? i == SSR_UPSCALE && differs(v, i)
               : s.resolve   ? i == SSR_SOFTEN && differs(v, i)
                             : i != SSR_SOFTEN && i != SSR_UPSCALE && i != SSR_UPSCALE_FILTER && i != SSR_ANGLE &&
                                   differs(v, i);
    if (!any)
    {
        s.active = 0;
        return;
    }
    const auto *ps = static_cast<const shader_desc *>(s.desc->subs[s.desc->pixel_shader].data);
    const auto *bytes = static_cast<const uint8_t *>(ps->code);
    std::vector<uint8_t> code(bytes, bytes + ps->code_size);
    if (!(s.water >= 0 ? make_upscale_copy(code, v, s.water)
          : s.resolve  ? make_resolve_copy(code, v)
                       : make_ssr_copy(code, v)))
    {
        s.active = 0;
        dd_log(reshade::log::level::warning, "SSR: the game's SSR shader differs from the known one. Left alone");
        return;
    }

    std::vector<pipeline_subobject> subs = s.desc->subs;
    shader_desc patched = *ps;
    patched.code = code.data();
    patched.code_size = code.size();
    subs[s.desc->pixel_shader].data = &patched;
    pipeline copy = {0};
    if (!s.dev->create_pipeline(s.layout, uint32_t(subs.size()), subs.data(), &copy))
    {
        s.active = 0;
        dd_log(reshade::log::level::warning, "SSR: the changed shader could not be made. The game's own stays");
        return;
    }
    s.made.push_back(copy.handle);
    s.active = copy.handle;
}

void apply_all()
{
    std::lock_guard lock(g_lock);
    for (slot &s : g_slots)
        if (s.game.load() != 0)
            apply_to(s);
}

void on_init_pipeline(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects,
                      pipeline pipe)
{
    if (dev->get_api() != device_api::d3d12)
        return;
    bool ours = false, resolve = false;
    int water = -1;
    for (uint32_t i = 0; i < count; ++i)
        if (subobjects[i].type == pipeline_subobject_type::pixel_shader && subobjects[i].data != nullptr)
        {
            const auto &shader = *static_cast<const shader_desc *>(subobjects[i].data);
            ours |= is_game_shader(shader);
            resolve |= is_resolve_shader(shader);
            if (water < 0)
                water = water_shader(shader);
        }
    if (!ours && !resolve && water < 0)
        return;

    auto *desc = new stored_desc();
    if (!desc->copy(count, subobjects))
    {
        char text[128];
        snprintf(text, sizeof(text),
                 "SSR: the game's SSR pipeline has an unexpected description (sub-object type %u). Left alone",
                 desc->unknown_type);
        delete desc;
        dd_log(reshade::log::level::warning, text);
        return;
    }
    std::lock_guard lock(g_lock);
    for (slot &s : g_slots)
    {
        if (s.game.load() != 0)
            continue;
        s.dev = dev;
        s.layout = layout;
        s.desc = desc;
        s.resolve = resolve;
        s.water = water;
        apply_to(s);
        s.game = pipe.handle;
        g_ready = true;
        if (g_logged.fetch_add(1) < 4)
            dd_log(reshade::log::level::info, "SSR: found the game's SSR shader");
        return;
    }
    delete desc;
    dd_log(reshade::log::level::warning, "SSR: the game made its SSR pipeline more often than expected");
}

void on_destroy_pipeline(device *dev, pipeline pipe)
{
    if (pipe.handle == 0)
        return;
    for (slot &s : g_slots)
    {
        if (s.game.load() != pipe.handle)
            continue;
        std::lock_guard lock(g_lock);
        s.game = 0;
        s.active = 0;
        for (uint64_t handle : s.made)
            dev->destroy_pipeline({handle});
        s.made.clear();
        delete s.desc;
        s.desc = nullptr;
    }
}

void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipe)
{
    if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::pixel_shader)) == 0)
        return;
    for (const slot &s : g_slots)
        if (s.game.load(std::memory_order_acquire) == pipe.handle)
        {
            const uint64_t active = s.active.load(std::memory_order_acquire);
            // Set on the D3D12 list directly: ReShade's bind_pipeline would also reset the primitive topology.
            if (active != 0)
                if (auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native()); native != nullptr)
                    native->SetPipelineState(reinterpret_cast<ID3D12PipelineState *>(active));
            diagnostics_bound(cmd, PASS_REFLECTIONS, active != 0);
            return;
        }
}
} // namespace

const param_def SSR_DEFS[SSR_PARAMS] = {
    {"Smoothing", "Smoothing", "Calms the water before it reflects. Higher flickers less\n"
     "but shows fewer ripples. The game uses 0.70.",
     0.70f, 0.5f, 1, "%.2f", false, false, false},
    {"Thickness", "Thickness", "Fills small gaps in reflections. Lower looks more natural\n"
     "with many steps. The game uses 2.0.",
     2, 1, 8, "%.1f", false, false, false},
    {"RayLength", "Ray length", "How far reflection rays reach. Higher reflects things farther from\n"
     "the surface. The steps spread over the ray. A longer ray needs more\nsteps for the same detail. The game uses 1.6.",
     1.6f, 0.4f, 32, "%.1f", false, true, true},
    {"Steps", "Steps", "How finely each reflection is traced. More gives sharper reflections\n"
     "of thin things like reeds and posts. The game uses 128.",
     128, 128, 4096, "%.0f", true, true, false},
    {"DistanceSmoothing", "Distance smoothing", "How much the water flattens with distance. Higher is calmer\n"
     "and steadier far away. The game uses 0.05.",
     0.05f, 0.005f, 0.5f, "%.3f", false, true, true},
    {"FarDetail", "Far ripple detail", "Higher keeps more ripple detail in far water by flattening it less.\n"
     "The game uses 0.010.",
     0.01f, 0.001f, 0.1f, "%.3f", false, true, true},
    {"RayCutoff", "Ray cutoff", "Rays bouncing back toward the camera get no reflection. Higher lets\n"
     "more of them through. More surfaces at steep angles then reflect. The\ngame uses 0.866.",
     0.86602f, 0.5f, 1, "%.3f", false, false, true},
    {"ShortThickness", "Short ray thickness", "Thickness for short rays, near what they reflect. Higher fills\n"
     "more holes there. The game uses 1.43.",
     1.0f / 0.7f, 1, 3.3f, "%.2f", false, false, true},
    {"FarThickness", "Far thickness", "Lower finds reflections on far water instead of ending early.\n"
     "Fewer steps need more of it. The game uses 0.018.",
     0.0175f, 0.002f, 0.1f, "%.3f", false, true, false},
    {"Jitter", "Jitter", "Hides banding in reflections. Fewer steps need more of it.\n"
     "The game uses 1.50.",
     1.5f, 0, 4, "%.2f", false, false, false},
    {"JitterRamp", "Jitter ramp", "How many steps the jitter takes to come in fully. Higher brings it\n"
     "in more gradually. The game uses 0.10.",
     0.1f, 0.01f, 1, "%.2f", false, true, true},
    {"StartOffset", "Start offset", "How far from the surface the first sample is. Higher avoids\n"
     "self-reflection specks but misses contact. The game uses 0.0125.",
     0.0125f, 0.001f, 0.1f, "%.4f", false, true, true},
    {"EdgeTop", "Edge reach", "How close to the top of the screen reflections reach before fading.\n"
     "Higher shows more of tall things in the water. The game uses 0.70.",
     0.70f, 0.7f, 1, "%.2f", false, false, false},
    {"EdgeSide", "Edge reach, sides", "Where reflections start to fade toward the sides of the screen.\n"
     "Higher keeps them closer to the edges. The game uses 0.95.",
     0.95f, 0.5f, 1, "%.2f", false, false, true},
    {"EdgeSideSharpness", "Edge fade sharpness, sides", "How quickly they fade at the sides. Higher is a\n"
     "shorter and harder fade. The game uses 20.",
     20, 1, 100, "%.0f", false, true, true},
    {"EdgeTopSharpness", "Edge fade sharpness, top and bottom", "How quickly they fade at the top and bottom.\n"
     "Higher is a shorter and harder fade. The game uses 3.3.",
     3.3333f, 0.5f, 50, "%.1f", false, true, true},
    {"Sky", "Sky", "How much of the sky on screen is reflected. The game uses 0: its\n"
     "reflection probes show the sky instead. Higher mirrors the screen's sky.",
     0, 0, 1, "%.2f", false, false, true},
    {"Intensity", "Intensity", "Brightness of screen-space reflections. The game uses 1.00. Above\n"
     "1 they can look too strong over the game's other reflections.",
     1, 0, 1.5f, "%.2f", false, false, true},
    {"MotionFix", "Motion fix", "Stops reflections from lagging behind their objects when the camera turns.\n0 is the game's. "
     "0.5 measured best. Past 0.75 they run ahead of their objects.",
     0, 0, 1, "%.2f", false, false, false},
    {"NoiseTexels", "Noise spread", "How finely the ray start noise spreads over the pixels. 64 is the game's.\n"
     "128 makes the grain finer.",
     64, 64, 128, "%.0f", true, false, false},
    {"Softening", "Softening", "Blends each reflection pixel with its neighbors. Hides the blocky look of the\n"
     "game's half-resolution reflections at some sharpness. 0 is the game's.",
     0, 0, 1, "%.2f", false, false, false},
    {"Refinement", "Hit refinement", "Extra depth checks to find the exact point a ray hits. 0 is the game's.\n"
     "Each one halves the error. Past 4 it is below a pixel.",
     0, 0, 4, "%.0f", true, false, false},
    {"SmoothUpscale", "Smooth upscale", "Reads the half-resolution reflection with a smooth filter instead of the\n"
     "game's plain one. Removes its 2-pixel steps. 0 is the game's.",
     0, 0, 1, "%.2f", false, false, false},
    {"UpscaleFilter", "Upscale filter", "The smooth upscale's filter. 1 is soft (4 reads).\n"
     "2 is sharp (13 reads) with a little more detail. Bright reflections at night can show steps.",
     1, 1, 2, "%.0f", true, false, true},
    {"ReflectionAngle", "Reflection angle", "How steeply the camera can look down before reflections fade out.\n"
     "The game fades them from 25 to 35 degrees.",
     35, 35, 80, "%.0f deg", false, false, false},
};

// The presets change only what was tested in game. Smoothing rises a little per preset. More steps give
// sharper thin reflections. Thickness is measured in steps. It is already tighter with more steps. It falls
// from the game's 2.0 to 1.0. Below 1.0 a surface crossed between two steps can be missed. Jitter falls as
// the steps rise. Edge reach rises in equal steps to 0.95, the game's value for the sides. Far thickness
// falls to 0.005 at 4096 steps. There it finds far reflections instead of the plain fallback. Lower gave
// noise in reflections of trees.
const float SSR_PRESET_VALUES[PRESETS][SSR_PARAMS] = {
    {0.70f, 2, 1.6f, 128, 0.05f, 0.01f, 0.86602f, 1.0f / 0.7f, 0.0175f, 1.5f, 0.1f, 0.0125f, 0.70f, 0.95f, 20,
     3.3333f, 0, 1, 0, 64, 0, 0, 0, 1, 35},
    {0.73f, 1.6f, 1.6f, 512, 0.05f, 0.01f, 0.86602f, 1.0f / 0.7f, 0.013f, 1.25f, 0.1f, 0.0125f, 0.78f, 0.95f, 20,
     3.3333f, 0, 1, 0.5f, 128, 0.17f, 2, 0.33f, 1, 43},
    {0.77f, 1.3f, 1.6f, 1024, 0.05f, 0.01f, 0.86602f, 1.0f / 0.7f, 0.009f, 1.0f, 0.1f, 0.0125f, 0.87f, 0.95f, 20,
     3.3333f, 0, 1, 0.5f, 128, 0.33f, 3, 0.67f, 1, 52},
    {0.80f, 1, 1.6f, 4096, 0.05f, 0.01f, 0.86602f, 1.0f / 0.7f, 0.005f, 0.5f, 0.1f, 0.0125f, 0.95f, 0.95f, 20,
     3.3333f, 0, 1, 0.5f, 128, 0.5f, 4, 1.0f, 1, 60},
};

void ssr_register()
{
    reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
}

void ssr_unregister()
{
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
}

void ssr_set(const float values[SSR_PARAMS])
{
    {
        std::lock_guard lock(g_lock);
        bool same = true;
        for (int i = 0; i < SSR_PARAMS; ++i)
        {
            const float v = std::clamp(values[i], SSR_DEFS[i].min, SSR_DEFS[i].max);
            same &= g_values[i] == v;
            g_values[i] = v;
        }
        ssr_angle_set(g_values[SSR_ANGLE]);
        static bool first = true;
        if (same && !first)
            return;
        first = false;
    }
    apply_all();
}

bool ssr_ready()
{
    return g_ready.load();
}
