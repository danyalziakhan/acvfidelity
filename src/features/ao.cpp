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
// Sample spread, min offset, min visibility and thin compensation follow ideas from Intel's XeGTAO
// (https://github.com/GameTechDev/XeGTAO) and BaBa's XeGTAO for ReShade
// (https://github.com/BarbatosAWLS/Reshade-Shaders/blob/main/Shaders/BaBa_XeGTAO.fx).
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
#include "light.h"
#include "log.h"
#include "shader_patch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
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
// mul_sat r0.z, r2.w, l(1 / 2): the average over the directions. AT_AVERAGE is its literal.
const size_t AT_AVERAGE_OP = 0x19b4;
// mul r4.w, r0.z, r4.w: how far out each step lands, in pixels.
const size_t AT_STEP_OFFSET = 0xabc;
// div r5.x, r5.x, r4.w of each side. r4.w is the sample's distance, next used by the falloff.
const size_t AT_DISTANCE[2] = {0xda4, 0x1080};
// mad r0.w, r0.w, l(pi / 16), CB1[0][3].z: each pixel of a 4x4 block turns its directions by its index (0 to
// 15) times pi / 16, plus a turn per frame. The N directions of a pixel are pi / N apart. With pi / 16, pixels
// 8 apart get the same directions even at 2. More directions repeat more. Spaced by pi / (16 N), the 16 pixels
// cover 16 N different directions over the half circle.
const size_t AT_PATTERN = 0x8ac;     // float pi / 16
// mov r7.yzw, l(0, -1000, -1000, 0): each direction starts its two horizons here.
const size_t AT_DIRECTION_START = 0xa40;
const uint32_t DIRECTION_START[8] = {0x08000036, 0x001000E2, 7, 0x00004002, 0, 0xC47A0000, 0xC47A0000, 0};
// The horizon update of each side: max r7.y, r4.w, r7.y and max r7.z, r4.w, r7.z. A sample can only raise
// the horizon. So every object counts as infinitely thick. Thin grass stems and poles shade like walls.
// The thickness heuristic of the GTAO paper (Jimenez et al. 2016) lets the horizon sink toward a lower sample
// by a share. A thin occluder's shade then fades behind it. A solid wall still keeps the horizon up.
//   mul r9.w, r7.y, l(1 - share)
//   mad r9.w, r4.w, l(share), r9.w
//   max r7.y, r4.w, r9.w
// r9.w is unused in the game's shader. The share applies at every step. So it adds up: (1 - share)^steps of
// a thin occluder's shade is left. Share 0 is the game's own.
// That alone also let shade fade behind pots, crates and rocks. The sink can't tell how wide an object was.
// So each side counts how many steps in a row stayed on the object that holds its horizon. The counts are
// r10.x and r10.z, reset per direction. A sample counts while it is within a tolerance of the horizon. Bumps
// on a surface then don't end the count. A sample that jumps far above the horizon in one step is a new and
// nearer object. Its count starts again at 1. A pole right after open ground then counts as narrow. The jump
// has to be large. A wall seen from its base rises a little at every step and must keep counting. Each side
// keeps that count for the horizon it holds in r10.y and r10.w. The horizon sinks only when that object was
// narrow. A grass stem lasts a step or two, a pot many.
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

// The game may create its AO pipeline more than once: one per root signature
// or after a device reset. The bind event reads `game` and `active` without
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
    std::vector<uint64_t> made; // every copy not destroyed yet, the active one included
    // The values the active copy was made for, so an unrelated change doesn't make it again.
    float built[AO_PARAMS] = {};
    bool built_valid = false;
    // Copies replaced by a newer one, and the frame after which they are destroyed. Command lists the game
    // recorded with them may still run for a few frames.
    std::vector<std::pair<uint64_t, uint64_t>> retired;
};
// Frames since start, counted at present. Replaced copies wait this many frames before they are destroyed.
std::atomic<uint64_t> g_frame{0};
const uint64_t RETIRE_FRAMES = 16;
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
        read_u32(code, AT_AVERAGE) != bits(0.5f) || read_u32(code, AT_PATTERN) != bits(pi / 16.0f) ||
        memcmp(code.data() + AT_POWER, POWER_GAME, sizeof(POWER_GAME)) != 0)
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
    write_u32(code, AT_PATTERN, bits(pi / (16.0f * float(slices))));

    // Everything below inserts code. Back to front: an insert moves everything after it.
    const float thin = v[AO_THIN], compensation = v[AO_THIN_COMPENSATION];
    if (thin > 0.0f && memcmp(code.data() + AT_DIRECTION_START, DIRECTION_START, sizeof(DIRECTION_START)) != 0)
        return false;
    if (v[AO_MIN_VISIBILITY] > 0.0f)
    {
        // max r0.z, r0.z, l(floor) after the average over the directions.
        const uint32_t game[7] = {0x07002038, 0x00100042, 0, 0x0010003A, 2, 0x00004001, bits(1.0f / float(slices))};
        if (memcmp(code.data() + AT_AVERAGE_OP, game, sizeof(game)) != 0)
            return false;
        const uint32_t floor[7] = {0x07000034, 0x00100042, 0, 0x0010002A, 0, 0x00004001, bits(v[AO_MIN_VISIBILITY])};
        if (!dxbc_insert(code, AT_AVERAGE_OP + sizeof(game), floor, 7))
            return false;
    }
    // The longest run of steps, in a row, that still counts as thin. A step spans about radius / steps.
    const float narrow = std::max(1.0f, std::round(float(steps) * v[AO_THIN_WIDTH] / v[AO_RADIUS])) + 0.5f;
    // How far above the horizon a step has to land to start a new object.
    const float jump = std::max(0.25f, 2.5f * v[AO_THIN_TOLERANCE]);
    for (int k = 1; k >= 0; --k)
    {
        if (thin > 0.0f)
        {
            const horizon_site &h = HORIZONS[k];
            const uint32_t game[7] = {0x07000034, h.horizon_dest, 7, 0x0010003A, 4, h.horizon_src, 7};
            if (memcmp(code.data() + h.at, game, sizeof(game)) != 0)
                return false;
            // This side's run count and held width: r10.x and r10.y, or r10.z and r10.w.
            const uint32_t run_dst = k == 0 ? 0x00100012 : 0x00100042, run_src = k == 0 ? 0x0010000A : 0x0010002A;
            const uint32_t wide_dst = k == 0 ? 0x00100022 : 0x00100082, wide_src = k == 0 ? 0x0010001A : 0x0010003A;
            const uint32_t tokens[] = {
                // add r9.y, horizon, l(-tolerance)
                0x07000000, 0x00100022, 9, h.horizon_src, 7, 0x00004001, bits(-v[AO_THIN_TOLERANCE]),
                // ge r9.w, r4.w, r9.y: this step is still on the object that holds the horizon
                0x0700001D, 0x00100082, 9, 0x0010003A, 4, 0x0010001A, 9,
                // add r9.x, run, l(1)
                0x07000000, 0x00100012, 9, run_src, 10, 0x00004001, bits(1.0f),
                // and run, r9.x, r9.w: one more in a row, or 0
                0x07000001, run_dst, 10, 0x0010000A, 9, 0x0010003A, 9,
                // add r9.x, horizon, l(jump)
                0x07000000, 0x00100012, 9, h.horizon_src, 7, 0x00004001, bits(jump),
                // lt r9.x, r9.x, r4.w: far above the horizon in one step, a new object
                0x07000031, 0x00100012, 9, 0x0010000A, 9, 0x0010003A, 4,
                // movc run, r9.x, l(1), run
                0x09000037, run_dst, 10, 0x0010000A, 9, 0x00004001, bits(1.0f), run_src, 10,
                // movc wide, r9.w, run, wide: the width of whatever holds the horizon
                0x09000037, wide_dst, 10, 0x0010003A, 9, run_src, 10, wide_src, 10,
                // mul r9.x, horizon, l(1 - share)
                0x07000038, 0x00100012, 9, h.horizon_src, 7, 0x00004001, bits(1.0f - thin),
                // mad r9.x, r4.w, l(share), r9.x: the sunk horizon
                0x09000032, 0x00100012, 9, 0x0010003A, 4, 0x00004001, bits(thin), 0x0010000A, 9,
                // lt r9.y, wide, l(narrow)
                0x07000031, 0x00100022, 9, wide_src, 10, 0x00004001, bits(narrow),
                // movc r9.x, r9.y, r9.x, horizon: sink only behind a narrow object
                0x09000037, 0x00100012, 9, 0x0010001A, 9, 0x0010000A, 9, h.horizon_src, 7,
                // max horizon, r4.w, r9.x
                0x07000034, h.horizon_dest, 7, 0x0010003A, 4, 0x0010000A, 9,
            };
            const size_t count = sizeof(tokens) / sizeof(tokens[0]);
            memcpy(code.data() + h.at, tokens, 7 * 4);
            if (!dxbc_insert(code, h.at + 7 * 4, tokens + 7, count - 7))
                return false;
        }
        if (compensation > 0.0f)
        {
            // After div r5.x, r5.x, r4.w (the horizon cosine), the distance in r4.w feeds the falloff. It is
            // measured again with the depth part of r9 stretched by 1 + compensation. Occluders far behind the
            // pixel then fall off sooner. Thin things in front of open ground stop hazing it.
            const uint32_t game[7] = {0x0700000E, 0x00100012, 5, 0x0010000A, 5, 0x0010003A, 4};
            if (memcmp(code.data() + AT_DISTANCE[k], game, sizeof(game)) != 0)
                return false;
            const uint32_t tokens[] = {
                // mul r9.z, r9.z, l(1 + compensation)
                0x07000038, 0x00100042, 9, 0x0010002A, 9, 0x00004001, bits(1.0f + compensation),
                // dp3 r4.w, r9.xyzx, r9.xyzx
                0x07000010, 0x00100082, 4, 0x00100246, 9, 0x00100246, 9,
                // sqrt r4.w, r4.w
                0x0500004B, 0x00100082, 4, 0x0010003A, 4,
            };
            if (!dxbc_insert(code, AT_DISTANCE[k] + sizeof(game), tokens, uint32_t(std::size(tokens))))
                return false;
        }
    }
    if (v[AO_SPREAD] != 1.0f || v[AO_MIN_OFFSET] != 0.0f)
    {
        // mul r4.w, r0.z, r4.w: step i (plus jitter) times the step length. The steps get spread as
        // ((i + jitter) / steps)^spread x radius + offset, in pixels. Above 1 they gather near the pixel, where
        // contact shade is. The offset keeps the first step off the pixel itself.
        const uint32_t game[7] = {0x07000038, 0x00100082, 4, 0x0010002A, 0, 0x0010003A, 4};
        if (memcmp(code.data() + AT_STEP_OFFSET, game, sizeof(game)) != 0)
            return false;
        const uint32_t tokens[] = {
            // mul r4.w, r4.w, l(1 / steps)
            0x07000038, 0x00100082, 4, 0x0010003A, 4, 0x00004001, bits(1.0f / float(steps)),
            // log r4.w, r4.w
            0x0500002F, 0x00100082, 4, 0x0010003A, 4,
            // mul r4.w, r4.w, l(spread)
            0x07000038, 0x00100082, 4, 0x0010003A, 4, 0x00004001, bits(v[AO_SPREAD]),
            // exp r4.w, r4.w
            0x05000019, 0x00100082, 4, 0x0010003A, 4,
            // mul r9.x, r0.z, l(steps): the radius in pixels
            0x07000038, 0x00100012, 9, 0x0010002A, 0, 0x00004001, bits(float(steps)),
            // mad r4.w, r4.w, r9.x, l(offset)
            0x09000032, 0x00100082, 4, 0x0010003A, 4, 0x0010000A, 9, 0x00004001, bits(v[AO_MIN_OFFSET]),
        };
        memcpy(code.data() + AT_STEP_OFFSET, tokens, sizeof(game));
        if (!dxbc_insert(code, AT_STEP_OFFSET + sizeof(game), tokens + 7, uint32_t(std::size(tokens)) - 7))
            return false;
    }
    if (thin > 0.0f)
    {
        // mov r10.xyzw, l(0, 0, 0, 0) after each direction's horizon start.
        const uint32_t reset[8] = {0x08000036, 0x001000F2, 10, 0x00004002, 0, 0, 0, 0};
        if (!dxbc_insert(code, AT_DIRECTION_START + sizeof(DIRECTION_START), reset, 8) ||
            !dxbc_set_temps(code, 11))
            return false;
    }
    update_checksum(code);
    return true;
}

void retire_active(slot &s)
{
    const uint64_t old = s.active.exchange(0);
    if (old != 0)
        s.retired.push_back({old, g_frame.load() + RETIRE_FRAMES});
}

// Under g_lock. Makes the copy for the current values. Goes back to the
// game's own shader when all the values it carries are the game's.
void apply_to(slot &s)
{
    const float *v = g_values;
    const bool blur = s.what != kind::ao;
    // The values this shader carries: the blur's or the AO pass's. The lighting's are light.cpp's.
    const int from = blur ? AO_BLUR_WIDTH : 0, to = blur ? AO_LIGHT_FIRST : AO_BLUR_WIDTH;
    bool same = s.built_valid;
    for (int i = from; i < to && same; ++i)
        same = s.built[i] == v[i];
    if (same)
        return;
    for (int i = from; i < to; ++i)
        s.built[i] = v[i];
    s.built_valid = true;
    if (all_game_values(v, from, to))
    {
        retire_active(s);
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
        retire_active(s);
        dd_log(reshade::log::level::warning, !made ? "AO: a changed shader could not be made. The game's own stays"
                                                   : "AO: D3D12 refused a changed shader. The game's own stays");
        return;
    }
    s.made.push_back(copy.handle);
    // In one step, so a bind on another thread gets either the old copy or the new one.
    const uint64_t old = s.active.exchange(copy.handle);
    if (old != 0)
        s.retired.push_back({old, g_frame.load() + RETIRE_FRAMES});
}

// The device goes away: every copy made on it goes with it.
void on_destroy_device(device *dev)
{
    std::lock_guard lock(g_lock);
    for (slot &s : g_slots)
    {
        if (s.dev != dev)
            continue;
        s.game = 0;
        s.active = 0;
        for (uint64_t handle : s.made)
            dev->destroy_pipeline({handle});
        s.made.clear();
        s.retired.clear();
        s.built_valid = false;
        s.code.clear();
        s.dev = nullptr;
    }
}

// Once a frame: destroys replaced copies once no command list can still use them.
void on_present(effect_runtime *)
{
    const uint64_t frame = ++g_frame;
    std::lock_guard lock(g_lock);
    for (slot &s : g_slots)
    {
        for (size_t i = 0; i < s.retired.size();)
        {
            if (s.retired[i].second > frame || s.dev == nullptr)
            {
                ++i;
                continue;
            }
            const uint64_t handle = s.retired[i].first;
            s.dev->destroy_pipeline({handle});
            s.made.erase(std::remove(s.made.begin(), s.made.end(), handle), s.made.end());
            s.retired.erase(s.retired.begin() + i);
        }
    }
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
        s.retired.clear();
        s.built_valid = false;
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

// GPU time grows with directions x steps and with the radius. Measured at 1920x1200 on the test PC in a forest:
// Game 0.13 ms. Fidelity I about 0.33 ms. Fidelity II about 0.75 ms. Fidelity III about 1.3 ms.
const param_def AO_DEFS[AO_PARAMS] = {
    {"Directions", "Directions", "How many directions each pixel checks for shade. The game uses 2.\n"
     "More gives steadier shade.",
     2, 2, 12, "%.0f", true, false, false},
    {"Steps", "Steps", "How many steps each direction takes. The game uses 5.\n"
     "More catches thin things like grass and posts.",
     5, 5, 32, "%.0f", true, false, false},
    {"Strength", "Strength", "How dark the shade gets. The game uses 100%.\n"
     "It shows most in shade and indoors.",
     100, 25, 200, "%.0f%%", true, false, false},
    {"Radius", "Radius", "How far from each spot shade is looked for. The game uses 2 m.\n"
     "Larger shades deep corners but can add dark halos.",
     2, 0.5f, 16, "%.2f m", false, true, false},
    {"ThinObjects", "Thin objects", "Lets shade fade behind thin things like grass and poles. The game uses 0.",
     0, 0, 0.01f, "%.4f", false, false, false},
    {"FalloffStart", "Falloff start", "The share of the radius where occluders begin to count less.\n"
     "The game uses 0.50. Higher keeps more shade from nearby objects.",
     0.5f, 0.25f, 2, "%.2f", false, false, false},
    {"FalloffRange", "Falloff range", "Over what share of the radius they fade out after that.\n"
     "The game uses 1.00. Higher gives softer and wider shade.",
     1, 0.1f, 4, "%.2f", false, true, true},
    {"MinRadius", "Min radius", "The smallest radius on screen in pixels at half resolution.\n"
     "The game uses 5. It keeps distant shade from vanishing.",
     5, 1, 64, "%.0f px", false, false, true},
    {"MaxRadius", "Max radius", "Keeps shade right in front of the camera from getting too wide.\n"
     "The game uses 128.",
     128, 128, 512, "%.0f px", false, true, false},
    {"MaxDistance", "Max distance", "No shade beyond this many meters. It fades out over the last eighth.\n"
     "The game uses 256. Far trees and hills keep some shade up to 1024.",
     256, 32, 1024, "%.0f m", false, true, false},
    {"ThinWidth", "Thin objects width", "Objects narrower than this count as thin for Thin objects.\n"
     "Wider ones keep their full shade.",
     0.25f, 0.05f, 2, "%.2f m", false, true, true},
    {"ThinTolerance", "Thin objects tolerance", "How far a step may dip below the horizon and still count as the same object.\n"
     "Higher keeps bumpy surfaces whole.",
     0.1f, 0, 0.5f, "%.3f", false, false, true},
    {"SampleSpread", "Sample spread", "How the steps spread out from each pixel. The game uses 1 for even spacing.\n"
     "Higher places more of them close by where contact shade forms.",
     1, 1, 3, "%.1f", false, false, false},
    {"MinOffset", "Min offset", "The first step lands at least this many pixels away at half resolution.\n"
     "The game uses 0. It keeps surfaces from shading themselves.",
     0, 0, 4, "%.1f px", false, false, false},
    {"MinVisibility", "Min visibility", "Limits how dark shade can get. The game uses 0.\n"
     "It prevents pitch black spots in tight gaps.",
     0, 0, 0.2f, "%.2f", false, false, false},
    {"ThinCompensation", "Thin compensation", "Occluders far behind a surface count less. The game uses 0.\n"
     "It reduces the haze grass and leaves put on open ground and snow.",
     0, 0, 8, "%.1f", false, false, false},
    {"BlurWidth", "Blur width", "Spacing of the blur's neighbors in pixels. The game uses 1.\n"
     "0 turns the blur off and shows a checker pattern up close.\n"
     "Wider is smoother but softer.",
     1, 0, 4, "%.0f", true, false, true},
    {"BlurDepth", "Blur edge", "How sharply depth differences stop the blur at edges. The game uses 40.\n"
     "Lower blurs across edges and adds halos. Higher keeps edges crisp.",
     40, 1, 400, "%.0f", false, true, true},
    {"BlurNormal", "Blur normal edge", "How sharply changes in surface direction stop the blur. The game uses 4.\n"
     "Higher keeps creases and corners crisp. Lower blurs across them.",
     4, 0.5f, 32, "%.1f", false, true, true},
    {"BounceLight", "Bounce light", "Strength of the light that bounces off nearby surfaces into shade.\n"
     "1 is the game's.",
     1, 0.5f, 3, "%.2f", false, false, false},
    {"SunlightOcclusion", "Sunlight occlusion", "Lets AO dim sunlight where it is strong: plant bases, gaps between\n"
     "stones. 0 is the game's.",
     0, 0, 1, "%.2f", false, false, false},
    {"SunlightCurve", "Sunlight occlusion curve", "The power on the AO in sunlight occlusion. Higher keeps it to deep\n"
     "creases and leaves soft shade alone. 3 is the default.",
     3, 1, 8, "%.1f", false, false, true},
    {"BakedShadeBlend", "Baked shade blend", "Combines AO with the material's own baked shade instead of keeping only\n"
     "the darker one. 0 is the game's.",
     0, 0, 1, "%.2f", false, false, false},
    {"SkyLightDay", "Sky light (day)", "Strength of the sky's light in shade by day. 1 is the game's.\n"
     "It fades to the night value at dusk.",
     1, 0.5f, 2, "%.2f", false, false, false},
    {"ContactShadow", "Contact shadow strength", "How dark the game's contact shadows get. 1 is the game's.",
     1, 0, 2, "%.2f", false, false, true},
    {"SkyLightNight", "Sky light (night)", "Strength of the sky's light in shade at night. 1 is the game's.",
     1, 0.5f, 2, "%.2f", false, false, false},
};

// Lighting: bounce, sunlight occlusion, its curve, baked shade blend, sky light by day, contact shadow strength,
// sky light at night.
const float AO_PRESET_VALUES[PRESETS][AO_PARAMS] = {
    {2, 5, 100, 2, 0, 0.5f, 1, 5, 128, 256, 0.25f, 0.1f, 1, 0, 0, 0, 1, 40, 4, 1, 0, 3, 0, 1, 1, 1},
    {3, 10, 100, 2, 0.002f, 0.67f, 1, 5, 213, 512, 0.25f, 0.1f, 1.4f, 1.3f, 0.03f, 2, 1, 40, 4, 1.25f, 0.17f, 3, 0.5f, 1.07f, 1, 0.9f},
    {4, 20, 100, 2, 0.004f, 0.83f, 1, 5, 299, 768, 0.25f, 0.1f, 1.4f, 1.3f, 0.03f, 2, 1, 40, 4, 1.5f, 0.33f, 3, 1, 1.13f, 1, 0.8f},
    {5, 30, 100, 2, 0.006f, 1.0f, 1, 5, 384, 1024, 0.25f, 0.1f, 1.4f, 1.3f, 0.03f, 2, 1, 40, 4, 1.75f, 0.5f, 3, 1, 1.2f, 1, 0.7f},
};

void ao_register()
{
    reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::register_event<reshade::addon_event::reshade_present>(on_present);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    light_register();
}

void ao_unregister()
{
    light_unregister();
    reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::unregister_event<reshade::addon_event::reshade_present>(on_present);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
}

void ao_set(const float values[AO_PARAMS])
{
    float clamped[AO_PARAMS];
    {
        std::lock_guard lock(g_lock);
        bool same = true;
        for (int i = 0; i < AO_PARAMS; ++i)
        {
            const float v = std::clamp(values[i], AO_DEFS[i].min, AO_DEFS[i].max);
            same &= g_values[i] == v;
            g_values[i] = v;
            clamped[i] = v;
        }
        static bool first = true;
        if (same && !first)
            return;
        first = false;
    }
    light_set(clamped);
    apply_all();
}

bool ao_ready()
{
    return g_ready.load();
}

