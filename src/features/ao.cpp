// The AO shaders are recognized by size and checksum. Only those exact builds
// are ever changed. Each place a setting lives is also checked against the
// game's code first.
//
// In GTAO, five literals make up the sample count: the slice count, the angle
// between slices, the step count, the step length and the average over the
// slices. The power, radius, falloff, pixel limits and distance are constant
// buffer reads. Each becomes a literal. No-ops pad it to keep the code size.
// In the blurs the neighbor offsets and the depth weight are literals already.
//
// Copies are made on the game's root signature when the game creates its
// pipelines and whenever a setting changes. A copy is bound right after the
// game binds its own. Nothing has to be restored after it. The game binds the
// next pipeline it needs itself. Copies live until the game destroys its own
// pipeline because recorded command lists may still use an older one.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "ao.h"
#include "diagnostics.h"
#include "log.h"
#include "shader_patch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

using namespace reshade::api;

namespace
{
const size_t SHADER_SIZE = 6980;
const uint8_t SHADER_CHECKSUM[16] = {0x67, 0xe0, 0x24, 0xda, 0x71, 0x24, 0xeb, 0x4f,
                                     0x6d, 0xf1, 0x01, 0x43, 0xef, 0xe9, 0x3d, 0xae};

// Byte offsets of the values. The game's own are in the comments.
const size_t AT_SLICES = 0x9c8;      // uint 2
const size_t AT_SLICE_ANGLE = 0xa04; // float pi / 2
const size_t AT_STEPS = 0xa7c;       // uint 5
const size_t AT_STEP_SCALE = 0x814;  // float 1 / 5
const size_t AT_AVERAGE = 0x19cc;    // float 1 / 2
// The horizon update of each side: max r7.y, r4.w, r7.y and max r7.z, r4.w, r7.z. A sample can only raise
// the horizon. So every object counts as infinitely thick, and thin grass stems and poles shade like walls.
// The thickness heuristic of the GTAO paper (Jimenez et al. 2016) lets the horizon sink toward a lower sample
// by a share. A thin occluder's shade then fades behind it. A solid wall still keeps the horizon up.
//   mul r9.w, r7.y, l(1 - share)
//   mad r9.w, r4.w, l(share), r9.w
//   max r7.y, r4.w, r9.w
// r9.w is unused in the game's shader. The share applies at every step, so it adds up: (1 - share)^steps of
// a thin occluder's shade is left. Share 0 is the game's own.
struct horizon_site
{
    size_t at;
    uint32_t horizon_dest, horizon_src; // r7.y or r7.z
};
const horizon_site HORIZONS[2] = {{0xe34, 0x00100022, 0x0010001A}, {0x1110, 0x00100042, 0x0010002A}};
// mul r0.z, r0.z, CB1[0][2].x (nine tokens) becomes
// mul r0.z, r0.z, l(power), nop, nop.
const size_t AT_POWER = 0x19e4;
const uint32_t POWER_GAME[9] = {0x9000038, 0x100042, 0, 0x10002a, 0, 0x30800a, 1, 0, 2};
const uint32_t NOP = 0x100003a;

// A constant buffer read that becomes a literal. The instruction is rewritten
// with an immediate and padded with no-ops to its old length. The game's
// tokens are checked first.
struct cb_site
{
    size_t at;
    uint32_t old_size;
    uint32_t game[10];
    uint32_t replaced[7]; // LIT where the value goes
    int literal;
};
const uint32_t LIT = 0;
// lt r0.z, r0.x, CB1[0][2].y: AO only nearer than this (meters).
const cb_site SITE_MAX_DISTANCE = {0x170, 9, {0x9000031, 0x100042, 0, 0x10000a, 0, 0x30801a, 1, 0, 2},
                                   {0x7000031, 0x100042, 0, 0x10000a, 0, 0x4001, LIT}, 6};
// div r0.z, CB1[0][3].x, r0.z: radius in meters over depth.
const cb_site SITE_RADIUS = {0x798, 9, {0x900000e, 0x100042, 0, 0x30800a, 1, 0, 3, 0x10002a, 0},
                             {0x700000e, 0x100042, 0, 0x4001, LIT, 0x10002a, 0}, 4};
// min r0.z, r0.z, CB1[0][3].y: largest radius in pixels.
const cb_site SITE_MAX_PIXELS = {0x7d8, 9, {0x9000033, 0x100042, 0, 0x10002a, 0, 0x30801a, 1, 0, 3},
                                 {0x7000033, 0x100042, 0, 0x10002a, 0, 0x4001, LIT}, 6};
// add r4.w, r4.w, -CB1[0][4].x (both sides): distance where falloff starts.
const cb_site SITE_FALLOFF_START[2] = {
    {0xdc0, 10, {0xa000000, 0x100082, 4, 0x10003a, 4, 0x8030800a, 0x41, 1, 0, 4},
     {0x7000000, 0x100082, 4, 0x10003a, 4, 0x4001, LIT}, 6},
    {0x109c, 10, {0xa000000, 0x100082, 4, 0x10003a, 4, 0x8030800a, 0x41, 1, 0, 4},
     {0x7000000, 0x100082, 4, 0x10003a, 4, 0x4001, LIT}, 6}};
// mul_sat r4.w, r4.w, CB1[0][4].y (both sides): 1 / falloff range.
const cb_site SITE_FALLOFF_SCALE[2] = {
    {0xde8, 9, {0x9002038, 0x100082, 4, 0x10003a, 4, 0x30801a, 1, 0, 4},
     {0x7002038, 0x100082, 4, 0x10003a, 4, 0x4001, LIT}, 6},
    {0x10c4, 9, {0x9002038, 0x100082, 4, 0x10003a, 4, 0x30801a, 1, 0, 4},
     {0x7002038, 0x100082, 4, 0x10003a, 4, 0x4001, LIT}, 6}};
// add r0.x, r0.x, -CB1[0][2].z and mul_sat r0.x, r0.x, CB1[0][2].w: the
// fade toward the max distance (start, 1 / length).
const cb_site SITE_FADE_START = {0x1a38, 10, {0xa000000, 0x100012, 0, 0x10000a, 0, 0x8030802a, 0x41, 1, 0, 2},
                                 {0x7000000, 0x100012, 0, 0x10000a, 0, 0x4001, LIT}, 6};
const cb_site SITE_FADE_SCALE = {0x1a60, 9, {0x9002038, 0x100012, 0, 0x10000a, 0, 0x30803a, 1, 0, 2},
                                 {0x7002038, 0x100012, 0, 0x10000a, 0, 0x4001, LIT}, 6};
// max r0.z, r0.z, l(5): smallest radius in pixels. Already a literal.
const size_t AT_MIN_PIXELS = 0x7bc + 6 * 4;

// The AO blur runs two passes over 4 neighbors each, at -2, -1, 1 and 2
// pixels. Neighbors are weighted down by depth difference x40. The horizontal
// pass is 2028F85040544798 (uint to uint) and the vertical one
// 4E503DE13B6BD3D0 (uint to unorm). The offsets sit in
// iadd r2.xyzw, vThreadID.xyxy, l(...) at 0x40c (-1, -2) and 0xcd8 (2, 1).
// They are tokens 5 and 7 (x) in the horizontal pass and 6 and 8 (y) in the
// vertical one. The depth weight is token 6 of a mul_sat at four places in
// both.
struct blur_desc
{
    size_t size;
    uint8_t checksum[16];
    int first_token;
};
const blur_desc BLURS[2] = {
    {5720, {0x88, 0xf0, 0xea, 0x40, 0x7f, 0x91, 0x33, 0x56, 0x9c, 0x00, 0xaf, 0xad, 0x3e, 0xd8, 0xaa, 0x18}, 5},
    {5628, {0x58, 0x2f, 0xe8, 0x27, 0x42, 0xbb, 0x8d, 0xd1, 0x7b, 0xa7, 0x75, 0x9c, 0x06, 0x6d, 0xfd, 0x3d}, 6},
};
const size_t BLUR_OFFSETS[2] = {0x40c, 0xcd8};
const int32_t BLUR_OFFSETS_GAME[2][2] = {{-1, -2}, {2, 1}};
const size_t BLUR_DEPTH[4] = {0x770, 0xbc8, 0x103c, 0x1478};
// The normal weight is max(dot(n, n'), 0) to the 4th power: two mul x, x, x in a row at the same four
// places in both blurs. For a different power they become log, mul by the power and two no-ops. An exp is
// inserted after them.
struct pow_site
{
    size_t at;
    uint32_t game[14];
    uint32_t replaced[14];
    int literal;
    uint32_t exp[5];
};
const pow_site BLUR_NORMAL[2][4] = {
    {
        {0x7e4, {0x7000038, 0x100022, 0x3, 0x10001a, 0x3, 0x10001a, 0x3, 0x7000038, 0x100022, 0x3, 0x10001a, 0x3, 0x10001a, 0x3}, {0x500002f, 0x100022, 0x3, 0x10001a, 0x3, 0x7000038, 0x100022, 0x3, 0x10001a, 0x3, 0x4001, 0x449a5000, 0x100003a, 0x100003a}, 11, {0x5000019, 0x100022, 0x3, 0x10001a, 0x3}},
        {0xc3c, {0x7000038, 0x100042, 0x2, 0x10002a, 0x2, 0x10002a, 0x2, 0x7000038, 0x100042, 0x2, 0x10002a, 0x2, 0x10002a, 0x2}, {0x500002f, 0x100042, 0x2, 0x10002a, 0x2, 0x7000038, 0x100042, 0x2, 0x10002a, 0x2, 0x4001, 0x449a5000, 0x100003a, 0x100003a}, 11, {0x5000019, 0x100042, 0x2, 0x10002a, 0x2}},
        {0x10b0, {0x7000038, 0x100042, 0x3, 0x10002a, 0x3, 0x10002a, 0x3, 0x7000038, 0x100042, 0x3, 0x10002a, 0x3, 0x10002a, 0x3}, {0x500002f, 0x100042, 0x3, 0x10002a, 0x3, 0x7000038, 0x100042, 0x3, 0x10002a, 0x3, 0x4001, 0x449a5000, 0x100003a, 0x100003a}, 11, {0x5000019, 0x100042, 0x3, 0x10002a, 0x3}},
        {0x14ec, {0x7000038, 0x100082, 0x0, 0x10003a, 0x0, 0x10003a, 0x0, 0x7000038, 0x100082, 0x0, 0x10003a, 0x0, 0x10003a, 0x0}, {0x500002f, 0x100082, 0x0, 0x10003a, 0x0, 0x7000038, 0x100082, 0x0, 0x10003a, 0x0, 0x4001, 0x449a5000, 0x100003a, 0x100003a}, 11, {0x5000019, 0x100082, 0x0, 0x10003a, 0x0}}
    },
    {
        {0x7e4, {0x7000038, 0x100012, 0x3, 0x10000a, 0x3, 0x10000a, 0x3, 0x7000038, 0x100012, 0x3, 0x10000a, 0x3, 0x10000a, 0x3}, {0x500002f, 0x100012, 0x3, 0x10000a, 0x3, 0x7000038, 0x100012, 0x3, 0x10000a, 0x3, 0x4001, 0x449a5000, 0x100003a, 0x100003a}, 11, {0x5000019, 0x100012, 0x3, 0x10000a, 0x3}},
        {0xc3c, {0x7000038, 0x100022, 0x2, 0x10001a, 0x2, 0x10001a, 0x2, 0x7000038, 0x100022, 0x2, 0x10001a, 0x2, 0x10001a, 0x2}, {0x500002f, 0x100022, 0x2, 0x10001a, 0x2, 0x7000038, 0x100022, 0x2, 0x10001a, 0x2, 0x4001, 0x449a5000, 0x100003a, 0x100003a}, 11, {0x5000019, 0x100022, 0x2, 0x10001a, 0x2}},
        {0x10b0, {0x7000038, 0x100022, 0x3, 0x10001a, 0x3, 0x10001a, 0x3, 0x7000038, 0x100022, 0x3, 0x10001a, 0x3, 0x10001a, 0x3}, {0x500002f, 0x100022, 0x3, 0x10001a, 0x3, 0x7000038, 0x100022, 0x3, 0x10001a, 0x3, 0x4001, 0x449a5000, 0x100003a, 0x100003a}, 11, {0x5000019, 0x100022, 0x3, 0x10001a, 0x3}},
        {0x14ec, {0x7000038, 0x100042, 0x0, 0x10002a, 0x0, 0x10002a, 0x0, 0x7000038, 0x100042, 0x0, 0x10002a, 0x0, 0x10002a, 0x0}, {0x500002f, 0x100042, 0x0, 0x10002a, 0x0, 0x7000038, 0x100042, 0x0, 0x10002a, 0x0, 0x4001, 0x449a5000, 0x100003a, 0x100003a}, 11, {0x5000019, 0x100042, 0x0, 0x10002a, 0x0}}
    },
};

// The current values, under g_lock.
float g_values[AO_PARAMS];

// The game may create its AO pipeline more than once (one per root signature
// or after a device reset). The bind event reads `game` and `active` without
// a lock. Everything else changes under g_lock.
enum class kind
{
    ao,
    blur_h,
    blur_v
};
struct slot
{
    kind what = kind::ao;
    std::atomic<uint64_t> game{0};
    std::atomic<uint64_t> active{0}; // 0: the game's own
    device *dev = nullptr;
    pipeline_layout layout = {0};
    std::vector<uint8_t> code;
    std::vector<uint64_t> made;
};
const int SLOTS = 16;
slot g_slots[SLOTS];
std::mutex g_lock;
std::atomic<bool> g_ready{false};
std::atomic<int> g_logged{0};

uint32_t read_u32(const std::vector<uint8_t> &code, size_t at)
{
    uint32_t v;
    memcpy(&v, code.data() + at, 4);
    return v;
}

void write_u32(std::vector<uint8_t> &code, size_t at, uint32_t v)
{
    memcpy(code.data() + at, &v, 4);
}

uint32_t bits(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

bool is_shader(const shader_desc &shader, size_t size, const uint8_t *checksum)
{
    return shader.code != nullptr && shader.code_size == size &&
           memcmp(static_cast<const uint8_t *>(shader.code) + 4, checksum, 16) == 0;
}

bool site_matches(const std::vector<uint8_t> &code, const cb_site &site)
{
    return memcmp(code.data() + site.at, site.game, site.old_size * 4) == 0;
}

void write_site(std::vector<uint8_t> &code, const cb_site &site, float value)
{
    uint32_t tokens[10];
    memcpy(tokens, site.replaced, sizeof(site.replaced));
    tokens[site.literal] = bits(value);
    for (uint32_t i = 7; i < site.old_size; ++i)
        tokens[i] = NOP;
    memcpy(code.data() + site.at, tokens, site.old_size * 4);
}

void update_checksum(std::vector<uint8_t> &code)
{
    uint32_t sum[4];
    dxbc_checksum(code.data(), code.size(), sum);
    memcpy(code.data() + 4, sum, 16);
}

bool all_game_values(const float *v, int from, int to)
{
    for (int i = from; i < to; ++i)
        if (v[i] != AO_DEFS[i].game)
            return false;
    return true;
}

bool make_blur_copy(std::vector<uint8_t> &code, const blur_desc &blur, const float *v)
{
    const int width = int(v[AO_BLUR_WIDTH]);
    for (int i = 0; i < 2; ++i)
        if (int32_t(read_u32(code, BLUR_OFFSETS[i] + blur.first_token * 4)) != BLUR_OFFSETS_GAME[i][0] ||
            int32_t(read_u32(code, BLUR_OFFSETS[i] + (blur.first_token + 2) * 4)) != BLUR_OFFSETS_GAME[i][1])
            return false;
    for (size_t at : BLUR_DEPTH)
        if (read_u32(code, at + 6 * 4) != bits(40.0f))
            return false;
    for (int i = 0; i < 2; ++i)
    {
        write_u32(code, BLUR_OFFSETS[i] + blur.first_token * 4, uint32_t(BLUR_OFFSETS_GAME[i][0] * width));
        write_u32(code, BLUR_OFFSETS[i] + (blur.first_token + 2) * 4, uint32_t(BLUR_OFFSETS_GAME[i][1] * width));
    }
    for (size_t at : BLUR_DEPTH)
        write_u32(code, at + 6 * 4, bits(v[AO_BLUR_DEPTH]));
    if (v[AO_BLUR_NORMAL] != AO_DEFS[AO_BLUR_NORMAL].game)
    {
        const pow_site(&sites)[4] = BLUR_NORMAL[&blur == &BLURS[0] ? 0 : 1];
        for (const pow_site &site : sites)
            if (memcmp(code.data() + site.at, site.game, sizeof(site.game)) != 0)
                return false;
        // Back to front. An insert moves everything after it.
        for (int i = 3; i >= 0; --i)
        {
            uint32_t tokens[14];
            memcpy(tokens, sites[i].replaced, sizeof(tokens));
            tokens[sites[i].literal] = bits(v[AO_BLUR_NORMAL]);
            memcpy(code.data() + sites[i].at, tokens, sizeof(tokens));
            if (!dxbc_insert(code, sites[i].at + sizeof(tokens), sites[i].exp, 5))
                return false;
        }
    }
    update_checksum(code);
    return true;
}

bool make_ao_copy(std::vector<uint8_t> &code, const float *v)
{
    const float pi = 3.14159265358979f;
    if (read_u32(code, AT_SLICES) != 2 || read_u32(code, AT_SLICE_ANGLE) != bits(pi / 2.0f) ||
        read_u32(code, AT_STEPS) != 5 || read_u32(code, AT_STEP_SCALE) != bits(0.2f) ||
        read_u32(code, AT_AVERAGE) != bits(0.5f) || memcmp(code.data() + AT_POWER, POWER_GAME, sizeof(POWER_GAME)) != 0)
        return false;
    // The game's constants stay unless a value that replaces them changed.
    if (!all_game_values(v, AO_RADIUS, AO_BLUR_WIDTH))
    {
        if (!site_matches(code, SITE_MAX_DISTANCE) || !site_matches(code, SITE_RADIUS) ||
            !site_matches(code, SITE_MAX_PIXELS) || !site_matches(code, SITE_FALLOFF_START[0]) ||
            !site_matches(code, SITE_FALLOFF_START[1]) || !site_matches(code, SITE_FALLOFF_SCALE[0]) ||
            !site_matches(code, SITE_FALLOFF_SCALE[1]) || !site_matches(code, SITE_FADE_START) ||
            !site_matches(code, SITE_FADE_SCALE) || read_u32(code, AT_MIN_PIXELS) != bits(5.0f))
            return false;
        const float radius = v[AO_RADIUS], distance = v[AO_MAX_DISTANCE];
        // The game fades out over the last eighth (224 to 256 m).
        write_site(code, SITE_MAX_DISTANCE, distance);
        write_site(code, SITE_FADE_START, -distance * 0.875f);
        write_site(code, SITE_FADE_SCALE, 1.0f / (distance * 0.125f));
        write_site(code, SITE_RADIUS, radius);
        write_site(code, SITE_MAX_PIXELS, v[AO_MAX_PIXELS]);
        write_u32(code, AT_MIN_PIXELS, bits(v[AO_MIN_PIXELS]));
        // The falloff scales with the radius as in the game. At 2 m it starts at 1 m and runs over 2 m.
        for (const cb_site &site : SITE_FALLOFF_START)
            write_site(code, site, -v[AO_FALLOFF_START] * radius);
        for (const cb_site &site : SITE_FALLOFF_SCALE)
            write_site(code, site, 1.0f / std::max(v[AO_FALLOFF_RANGE] * radius, 0.01f));
    }
    const uint32_t slices = uint32_t(v[AO_DIRECTIONS]), steps = uint32_t(v[AO_STEPS]);
    write_u32(code, AT_SLICES, slices);
    write_u32(code, AT_SLICE_ANGLE, bits(pi / float(slices)));
    write_u32(code, AT_STEPS, steps);
    write_u32(code, AT_STEP_SCALE, bits(1.0f / float(steps)));
    write_u32(code, AT_AVERAGE, bits(1.0f / float(slices)));
    const uint32_t power[9] = {0x7000038, 0x100042, 0, 0x10002a, 0, 0x4001, bits(v[AO_STRENGTH] / 100.0f), NOP, NOP};
    memcpy(code.data() + AT_POWER, power, sizeof(power));
    const float thin = v[AO_THIN];
    if (thin > 0.0f)
    {
        // Back to front: an insert moves everything after it.
        for (int k = 1; k >= 0; --k)
        {
            const horizon_site &h = HORIZONS[k];
            const uint32_t game[7] = {0x07000034, h.horizon_dest, 7, 0x0010003A, 4, h.horizon_src, 7};
            if (memcmp(code.data() + h.at, game, sizeof(game)) != 0)
                return false;
            const uint32_t keep[7] = {0x07000038, 0x00100082, 9, h.horizon_src, 7, 0x00004001, bits(1.0f - thin)};
            const uint32_t rest[16] = {0x09000032, 0x00100082, 9, 0x0010003A, 4, 0x00004001, bits(thin),
                                       0x0010003A, 9, 0x07000034, h.horizon_dest, 7, 0x0010003A, 4, 0x0010003A, 9};
            memcpy(code.data() + h.at, keep, sizeof(keep));
            if (!dxbc_insert(code, h.at + sizeof(keep), rest, 16))
                return false;
        }
    }
    update_checksum(code);
    return true;
}

// Under g_lock. Makes the copy for the current values. Goes back to the
// game's own shader when all the values it carries are the game's.
void apply_to(slot &s)
{
    const float *v = g_values;
    const bool blur = s.what != kind::ao;
    if (blur ? all_game_values(v, AO_BLUR_WIDTH, AO_PARAMS) : all_game_values(v, 0, AO_BLUR_WIDTH))
    {
        s.active = 0;
        return;
    }
    std::vector<uint8_t> code = s.code;
    const bool made = blur ? make_blur_copy(code, BLURS[s.what == kind::blur_h ? 0 : 1], v) : make_ao_copy(code, v);
    // After the patch: inserted instructions can move and grow the code.
    pipeline copy = {0};
    shader_desc desc = {};
    desc.code = code.data();
    desc.code_size = code.size();
    pipeline_subobject sub = {pipeline_subobject_type::compute_shader, 1, &desc};
    if (!made || !s.dev->create_pipeline(s.layout, 1, &sub, &copy))
    {
        s.active = 0;
        dd_log(reshade::log::level::warning, !made ? "AO: a changed shader could not be made. The game's own stays"
                                                   : "AO: D3D12 refused a changed shader. The game's own stays");
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
    const shader_desc *shader = nullptr;
    for (uint32_t i = 0; i < count; ++i)
        if (subobjects[i].type == pipeline_subobject_type::compute_shader && subobjects[i].data != nullptr)
            shader = static_cast<const shader_desc *>(subobjects[i].data);
    if (shader == nullptr)
        return;
    kind what;
    if (is_shader(*shader, SHADER_SIZE, SHADER_CHECKSUM))
        what = kind::ao;
    else if (is_shader(*shader, BLURS[0].size, BLURS[0].checksum))
        what = kind::blur_h;
    else if (is_shader(*shader, BLURS[1].size, BLURS[1].checksum))
        what = kind::blur_v;
    else
        return;

    std::lock_guard lock(g_lock);
    for (slot &s : g_slots)
    {
        if (s.game.load() != 0)
            continue;
        const auto *bytes = static_cast<const uint8_t *>(shader->code);
        s.what = what;
        s.dev = dev;
        s.layout = layout;
        s.code.assign(bytes, bytes + shader->code_size);
        apply_to(s);
        s.game = pipe.handle;
        if (what != kind::ao)
        {
            dd_log(reshade::log::level::info, what == kind::blur_h ? "AO: found the game's horizontal AO blur"
                                                                   : "AO: found the game's vertical AO blur");
            return;
        }
        g_ready = true;
        if (g_logged.fetch_add(1) < 4)
            dd_log(reshade::log::level::info, "AO: found the game's AO shader");
        return;
    }
    dd_log(reshade::log::level::warning, "AO: the game made its AO pipeline more often than expected");
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
        // Lists the game recorded with the copies have run by the time it
        // destroys its own pipeline.
        for (uint64_t handle : s.made)
            dev->destroy_pipeline({handle});
        s.made.clear();
        s.code.clear();
    }
}

void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipe)
{
    if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::compute_shader)) == 0)
        return;
    for (const slot &s : g_slots)
        if (s.game.load(std::memory_order_acquire) == pipe.handle)
        {
            const uint64_t active = s.active.load(std::memory_order_acquire);
            if (active != 0)
                cmd->bind_pipeline(pipeline_stage::all_compute, {active});
            diagnostics_bound(cmd, s.what == kind::ao ? PASS_AO : s.what == kind::blur_h ? PASS_AO_BLUR_H : PASS_AO_BLUR_V,
                              active != 0);
            return;
        }
}
} // namespace

// GPU time grows with directions x steps and with the radius. Measured at 1920x1080 on the test PC with the
// 0.1.0 presets: Game 0.14 ms. Fidelity I 0.53 ms. Fidelity II 1.39 ms. Fidelity III 2.49 ms.
const param_def AO_DEFS[AO_PARAMS] = {
    {"Directions", "Directions", "How many directions each pixel checks for shading.\n"
     "More gives steadier shading. The game uses 2.",
     2, 2, 12, "%.0f", true, false, false},
    {"Steps", "Steps", "How finely each direction is checked.\n"
     "More catches thin things like grass and posts. The game uses 5.",
     5, 5, 32, "%.0f", true, false, false},
    {"Strength", "Strength", "How dark the shading is. 100% is the game's own.\n"
     "It shows most in shade and indoors.",
     100, 25, 200, "%.0f%%", true, false, false},
    {"Radius", "Radius", "How far from each spot shading is looked for. The game uses 2 m.\n"
     "Larger shades deep corners. Too large adds dark halos.",
     2, 0.5f, 16, "%.2f m", false, true, false},
    {"ThinObjects", "Thin objects", "Lets shade fade behind thin things like grass and poles. 0 is the game's.\n"
     "It adds up over the steps. Small values already go far.",
     0, 0, 0.01f, "%.4f", false, false, false},
    {"FalloffStart", "Falloff start", "Where occluders start to count less, as a share of the radius.\n"
     "The game uses 0.50. Higher keeps far occluders stronger.",
     0.5f, 0.25f, 2, "%.2f", false, false, true},
    {"FalloffRange", "Falloff range", "Over what share of the radius they fade out after that. The game uses\n"
     "1.00. Higher gives softer, wider shading.",
     1, 0.1f, 4, "%.2f", false, true, true},
    {"MinRadius", "Min radius", "The radius on screen is at least this many pixels (half resolution).\n"
     "The game uses 5. It keeps distant AO from vanishing.",
     5, 1, 64, "%.0f px", false, false, true},
    {"MaxRadius", "Max radius", "Keeps shading right in front of the camera from getting too wide.\n"
     "The game uses 128.",
     128, 128, 512, "%.0f px", false, true, false},
    {"MaxDistance", "Max distance", "No AO beyond this many meters. It fades out over the last eighth.\n"
     "256 is the game's and the most. The game's lighting fades AO out past it.",
     256, 32, 256, "%.0f m", false, true, true},
    {"BlurWidth", "Blur width", "Spacing of the blur's neighbors, in pixels. 1 is the game's. 0 turns\n"
     "the blur off: a checker pattern shows on near surfaces.\n"
     "Wider is smoother but softer.",
     1, 0, 4, "%.0f", true, false, true},
    {"BlurDepth", "Blur edge", "How sharply depth differences stop the blur at edges. 40 is the game's.\n"
     "Lower blurs across edges and gives halos. Higher keeps edges crisp.",
     40, 1, 400, "%.0f", false, true, true},
    {"BlurNormal", "Blur normal edge", "How sharply differences in surface direction stop the blur. 4 is the\n"
     "game's. Higher keeps creases and corners crisp. Lower blurs across them.",
     4, 0.5f, 32, "%.1f", false, true, true},
};

const float AO_PRESET_VALUES[PRESETS][AO_PARAMS] = {
    {2, 5, 100, 2, 0, 0.5f, 1, 5, 128, 256, 1, 40, 4},
    {4, 10, 100, 2.5f, 0.0015f, 0.5f, 1, 5, 192, 256, 1, 40, 4},
    {6, 20, 100, 3.0f, 0.003f, 0.5f, 1, 5, 384, 256, 1, 40, 4},
    {8, 30, 100, 3.5f, 0.0045f, 0.5f, 1, 5, 512, 256, 1, 40, 4},
};

void ao_register()
{
    reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
}

void ao_unregister()
{
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
}

void ao_set(const float values[AO_PARAMS])
{
    {
        std::lock_guard lock(g_lock);
        bool same = true;
        for (int i = 0; i < AO_PARAMS; ++i)
        {
            const float v = std::clamp(values[i], AO_DEFS[i].min, AO_DEFS[i].max);
            same &= g_values[i] == v;
            g_values[i] = v;
        }
        static bool first = true;
        if (same && !first)
            return;
        first = false;
    }
    apply_all();
}

bool ao_ready()
{
    return g_ready.load();
}

