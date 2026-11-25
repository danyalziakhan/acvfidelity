// The scene lighting applies the AO as min(material AO, screen AO) to the
// ambient light only. The patches here, each 0 or 1 at the game's look:
// - Baked shade blend: the min becomes min x lerp(1, max, k). At 1 it is the
//   product of both. AO then also shows where the material has baked shade.
// - Sunlight occlusion: the sun shadow is multiplied by 1 + k (ms - 1), with
//   the micro-shadow ms = sat(|NdotL| + 2 ao^p - 1) (Chan 2018, "Material
//   advances in Call of Duty: WWII"). p = 3 leaves soft outer AO alone.
// - Bounce light: the probe bounce light times k, right after the game's own
//   multiplier CB0[0][103].z.
// - Sky fill and contact shadow strength, debug only.
//
// Sites are found by pattern on the decoded instructions. Every reader is
// bounds checked. Anything unexpected leaves the shader alone. New code reads
// only registers the shader already uses, plus one new temp.

#include "light_patch.h"
#include "shader_patch.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace
{
const uint32_t HALLMARK[2] = {0x400295EA, 0x40305532}; // 2.0404 and 2.7552 of the multi-bounce AO curve

enum : uint32_t
{
    OP_ADD = 0,
    OP_DP2 = 15,
    OP_DP3 = 16,
    OP_DP4 = 17,
    OP_EXP = 25,
    OP_LD = 45,
    OP_LD_MS = 46,
    OP_LOG = 47,
    OP_MAD = 50,
    OP_MIN = 51,
    OP_MAX = 52,
    OP_CUSTOMDATA = 53,
    OP_MOV = 54,
    OP_MUL = 56,
    OP_DCL_RESOURCE = 88,
    OP_DCL_TEMPS = 104,
    OP_DIV = 14,
    OP_SAMPLE_L = 72,
    OP_DCL_RESOURCE_RAW = 161,
    OP_DCL_RESOURCE_STRUCTURED = 162,
    OP_LD_STRUCTURED = 167,
    OP_FTOU = 28,
    OP_STORE_UAV_TYPED = 164,
    OP_CASE = 6,
    OP_DEFAULT = 10,
    OP_ELSE = 18,
    OP_ENDIF = 21,
    OP_ENDLOOP = 22,
    OP_ENDSWITCH = 23,
    OP_IF = 31,
    OP_LT = 49,
    OP_LOOP = 48,
    OP_AND = 1,
    OP_GE = 29,
    OP_MIN_ = 51,
    OP_MOVC = 55,
    OP_SWITCH = 76,
};
enum : uint32_t
{
    T_TEMP = 0,
    T_IMM32 = 4,
    T_IMM64 = 5,
    T_RESOURCE = 7,
    T_CB = 8,
};
enum
{
    AFTER,
    REPLACE,
    BEFORE
};

int comp_index(char c)
{
    return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : 3;
}

// A temp register named inside a relative index, with its components as printed.
struct ref
{
    uint32_t reg;
    std::string comps;
};

struct operand
{
    uint32_t type = 0, ncomp = 0, mode = 0, sel = 0, modifier = 0, dims = 0;
    uint32_t index[3] = {};
    bool simple = true; // every index is an immediate
    size_t begin = 0, end = 0; // in the program's tokens
    std::vector<ref> nested;
};

struct instr
{
    size_t at = 0;
    uint32_t op = 0;
    bool sat = false;
    bool plain = false; // no extended opcode tokens and no control bits besides saturate
    std::vector<operand> ops;
};

struct resource
{
    uint32_t id, lower, upper, space;
};

struct program
{
    size_t chunk = 0, stream = 0; // byte offsets of the code chunk and its version token
    uint32_t length = 0;          // tokens, version and length included
    std::vector<uint32_t> tokens; // the instructions
    std::vector<instr> ins;
    std::vector<size_t> sizes; // tokens of each instruction
    size_t temps_at = SIZE_MAX; // instruction index of dcl_temps
    uint32_t temps = 0;
    std::vector<resource> resources;
    // Every shader resource declaration (typed, raw, structured), with the stride for structured ones.
    struct srv
    {
        size_t index; // instruction
        uint32_t op, id, lower, upper, space, stride;
    };
    std::vector<srv> srvs;
    bool srvs_unknown = false; // a raw or structured declaration of an unexpected form
};

// The components as the disassembler prints them: a mask, four swizzle letters or one.
std::string comps(const operand &o)
{
    static const char XYZW[] = "xyzw";
    std::string s;
    if (o.ncomp != 4)
        return s;
    if (o.mode == 0)
    {
        for (int i = 0; i < 4; ++i)
            if (o.sel >> i & 1)
                s += XYZW[i];
    }
    else if (o.mode == 1)
    {
        for (int i = 0; i < 4; ++i)
            s += XYZW[o.sel >> (2 * i) & 3];
    }
    else
        s += XYZW[o.sel & 3];
    return s;
}

bool parse_operand(const uint32_t *t, size_t &p, size_t end, operand &o, int depth)
{
    if (p >= end || depth > 2)
        return false;
    o.begin = p;
    const uint32_t tok = t[p++];
    const uint32_t n = tok & 3;
    if (n == 3)
        return false;
    o.ncomp = n == 2 ? 4 : n;
    if (o.ncomp == 4)
    {
        o.mode = tok >> 2 & 3;
        o.sel = tok >> 4 & 0xFF;
        if (o.mode == 3)
            return false;
    }
    o.type = tok >> 12 & 0xFF;
    o.dims = tok >> 20 & 3;
    for (bool more = (tok >> 31) != 0; more;)
    {
        if (p >= end)
            return false;
        const uint32_t e = t[p++];
        // Only a modifier: no min precision or non-uniform flags.
        if ((e & 0x3F) != 1 || (e & 0x7FFFC000) != 0)
            return false;
        o.modifier = e >> 6 & 0xFF;
        more = (e >> 31) != 0;
    }
    if (o.type == T_IMM32)
    {
        if ((o.ncomp != 1 && o.ncomp != 4) || o.dims != 0 || end - p < o.ncomp)
            return false;
        p += o.ncomp;
        o.end = p;
        return true;
    }
    if (o.type == T_IMM64)
        return false;
    for (uint32_t i = 0; i < o.dims; ++i)
    {
        const uint32_t rep = tok >> (22 + 3 * i) & 7;
        if (rep > 4)
            return false;
        if (rep == 0 || rep == 3)
        {
            if (p >= end)
                return false;
            o.index[i] = t[p++];
        }
        else if (rep == 1 || rep == 4)
        {
            if (end - p < 2)
                return false;
            p += 2;
            o.simple = false;
        }
        if (rep >= 2)
        {
            operand r;
            if (!parse_operand(t, p, end, r, depth + 1))
                return false;
            o.simple = false;
            if (r.type == T_TEMP && r.dims == 1)
                o.nested.push_back({r.index[0], comps(r)});
            o.nested.insert(o.nested.end(), r.nested.begin(), r.nested.end());
        }
    }
    o.end = p;
    return true;
}

uint32_t read32(const uint8_t *code, size_t at)
{
    uint32_t v;
    memcpy(&v, code + at, 4);
    return v;
}

const char *parse(const uint8_t *code, size_t size, program &pr)
{
    if (code == nullptr || size < 32 || size % 4 != 0 || size > (64u << 20) || memcmp(code, "DXBC", 4) != 0)
        return "not a DXBC container";
    if (read32(code, 24) != size)
        return "container size wrong";
    uint32_t sum[4];
    dxbc_checksum(code, size, sum);
    if (memcmp(code + 4, sum, 16) != 0)
        return "checksum wrong";
    const uint32_t chunks = read32(code, 28);
    if (chunks == 0 || chunks > 64 || 32 + size_t(chunks) * 4 > size)
        return "chunk count wrong";
    int found = 0;
    for (uint32_t i = 0; i < chunks; ++i)
    {
        const size_t at = read32(code, 32 + size_t(i) * 4);
        if (at < 32 + size_t(chunks) * 4 || at % 4 != 0 || at + 8 > size || read32(code, at + 4) > size - at - 8)
            return "chunk outside the container";
        if (memcmp(code + at, "SHEX", 4) == 0 || memcmp(code + at, "SHDR", 4) == 0)
        {
            pr.chunk = at;
            ++found;
        }
    }
    if (found != 1)
        return "no code chunk";
    pr.stream = pr.chunk + 8;
    const uint32_t chunk_size = read32(code, pr.chunk + 4);
    if (chunk_size < 8)
        return "code chunk too small";
    const uint32_t version = read32(code, pr.stream);
    if ((version >> 16) != 0 || (version >> 4 & 0xF) != 5)
        return "not a shader model 5 pixel shader";
    pr.length = read32(code, pr.stream + 4);
    if (pr.length < 2 || size_t(pr.length) * 4 > chunk_size)
        return "code length wrong";
    pr.tokens.resize(pr.length - 2);
    if (!pr.tokens.empty())
        memcpy(pr.tokens.data(), code + pr.stream + 8, pr.tokens.size() * 4);

    const uint32_t *t = pr.tokens.data();
    const size_t n = pr.tokens.size();
    for (size_t p = 0; p < n;)
    {
        const uint32_t tok = t[p];
        const uint32_t op = tok & 0x7FF;
        const size_t len = op == OP_CUSTOMDATA ? (p + 1 < n ? t[p + 1] : 0) : tok >> 24 & 0x7F;
        if (len == 0 || len > n - p)
            return "malformed instruction length";
        instr in;
        in.at = p;
        in.op = op;
        const bool decl = op == OP_CUSTOMDATA || (op >= 88 && op <= 106) || (op >= 143 && op <= 162);
        if (op == OP_DCL_TEMPS)
        {
            if (len != 2 || pr.temps_at != SIZE_MAX)
                return "malformed dcl_temps";
            pr.temps_at = pr.ins.size();
            pr.temps = t[p + 1];
        }
        else if (op == OP_DCL_RESOURCE)
        {
            // Shader model 5.1: the range [id][lower][upper], the return type, the space.
            operand o;
            size_t q = p + 1;
            if (!parse_operand(t, q, p + len, o, 0) || o.type != T_RESOURCE || o.dims != 3 || !o.simple ||
                p + len - q != 2)
                return "malformed resource declaration";
            pr.resources.push_back({o.index[0], o.index[1], o.index[2], t[q + 1]});
            pr.srvs.push_back({pr.ins.size(), op, o.index[0], o.index[1], o.index[2], t[q + 1], 0});
        }
        else if (op == OP_DCL_RESOURCE_RAW || op == OP_DCL_RESOURCE_STRUCTURED)
        {
            // [range][lower][upper], the stride for structured ones, the space. Only the surface reflections read these.
            operand o;
            size_t q = p + 1;
            const size_t rest = op == OP_DCL_RESOURCE_STRUCTURED ? 2 : 1;
            if (parse_operand(t, q, p + len, o, 0) && o.type == T_RESOURCE && o.dims == 3 && o.simple &&
                p + len - q == rest)
                pr.srvs.push_back({pr.ins.size(), op, o.index[0], o.index[1], o.index[2], t[p + len - 1],
                                   rest == 2 ? t[q] : 0});
            else
                pr.srvs_unknown = true;
        }
        else if (!decl)
        {
            size_t q = p + 1;
            bool ext = (tok >> 31) != 0;
            in.plain = !ext && (tok & 0x00FFD800) == 0;
            in.sat = (tok >> 13 & 1) != 0;
            while (ext)
            {
                if (q >= p + len)
                    return "malformed extended opcode";
                const uint32_t e = t[q++];
                // Sample controls, resource dimension, return type. Nothing else is known here.
                if ((e & 0x3F) < 1 || (e & 0x3F) > 3)
                    return "unknown extended opcode";
                ext = (e >> 31) != 0;
            }
            while (q < p + len)
            {
                operand o;
                if (!parse_operand(t, q, p + len, o, 0))
                    return "malformed operand";
                in.ops.push_back(std::move(o));
            }
        }
        pr.ins.push_back(std::move(in));
        pr.sizes.push_back(len);
        p += len;
    }
    if (pr.temps_at == SIZE_MAX)
        return "no dcl_temps";
    if (pr.temps >= 4000)
        return "too many temps";
    return nullptr;
}

// ---------------------------------------------------------------- patterns

bool is_temp(const operand &o)
{
    return o.type == T_TEMP && o.dims == 1 && o.simple && o.ncomp == 4;
}

bool temp_is(const operand &o, uint32_t reg, const std::string &c)
{
    return is_temp(o) && o.modifier == 0 && o.index[0] == reg && comps(o) == c;
}

// One word in the disassembly: no relative index and no four-value literal.
bool one_word(const operand &o)
{
    return o.simple && !(o.type == T_IMM32 && o.ncomp == 4);
}

bool cb_is(const operand &o, uint32_t id, uint32_t slot, uint32_t at)
{
    return o.type == T_CB && o.dims == 3 && o.simple && o.ncomp == 4 && o.index[0] == id && o.index[1] == slot &&
           o.index[2] == at;
}

bool four_same(const std::string &s)
{
    return s.size() == 4 && s[1] == s[0] && s[2] == s[0] && s[3] == s[0];
}

bool arith(const instr &in, uint32_t op, size_t operands)
{
    return in.op == op && in.plain && !in.sat && in.ops.size() == operands;
}

// Writes the component: a temp in mask mode is always a destination.
bool writes(const instr &in, uint32_t reg, char c)
{
    for (const operand &o : in.ops)
        if (is_temp(o) && o.mode == 0 && o.index[0] == reg && comps(o).find(c) != std::string::npos)
            return true;
    return false;
}

bool reads_whole_swizzle(uint32_t op)
{
    return (op >= OP_DP2 && op <= OP_DP4) || op == OP_LD || op == OP_LD_MS || (op >= 69 && op <= 74) ||
           (op >= 109 && op <= 111) || (op >= 126 && op <= 128) || op == 163 || op == 165 || op == 167;
}

// Reads or writes the component. The first operand counts as written when it
// is a temp. Sources count the swizzle letters at the written positions.
bool touches(const instr &in, uint32_t reg, char c)
{
    if (in.ops.empty())
        return false;
    const operand &d = in.ops[0];
    std::string mask = "xyzw";
    size_t first = 0;
    if (is_temp(d))
    {
        mask = comps(d);
        if (d.index[0] == reg && mask.find(c) != std::string::npos)
            return true;
        first = 1;
    }
    const bool whole = reads_whole_swizzle(in.op);
    auto uses = [&](uint32_t r, const std::string &swz) {
        if (r != reg)
            return false;
        std::string used;
        if (swz.size() == 1 || whole)
            used = swz;
        else
            for (char ch : mask)
                if (size_t(comp_index(ch)) < swz.size())
                    used += swz[comp_index(ch)];
        return used.find(c) != std::string::npos;
    };
    for (size_t k = first; k < in.ops.size(); ++k)
    {
        const operand &o = in.ops[k];
        if (is_temp(o) && uses(o.index[0], comps(o)))
            return true;
        for (const ref &r : o.nested)
            if (uses(r.reg, r.comps))
                return true;
    }
    return false;
}

// How often a temp is named in an instruction.
int mentions(const instr &in, uint32_t reg)
{
    int count = 0;
    for (const operand &o : in.ops)
    {
        count += is_temp(o) && o.index[0] == reg;
        for (const ref &r : o.nested)
            count += r.reg == reg;
    }
    return count;
}

// ---------------------------------------------------------------- encoding

using tokens = std::vector<uint32_t>;

uint32_t bits(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

tokens dst(uint32_t reg, const std::string &mask)
{
    uint32_t m = 0;
    for (char c : mask)
        m |= 1u << comp_index(c);
    return {0x00100002u | m << 4, reg};
}

tokens sel(uint32_t reg, char c, uint32_t modifier = 0)
{
    const uint32_t t = 0x0010000Au | uint32_t(comp_index(c)) << 4;
    if (modifier != 0)
        return {t | 0x80000000u, 1u | modifier << 6, reg};
    return {t, reg};
}

tokens swz(uint32_t reg, const std::string &s)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= uint32_t(comp_index(s[std::min<size_t>(i, s.size() - 1)])) << (2 * i);
    return {0x00100006u | v << 4, reg};
}

// A swizzled temp, negated.
tokens swz_neg(uint32_t reg, const std::string &s)
{
    tokens t = swz(reg, s);
    return {t[0] | 0x80000000u, 1u | 1u << 6, t[1]};
}

tokens lit(float f)
{
    return {0x4001, bits(f)};
}

tokens lit4(float a, float b, float c, float d)
{
    return {0x4002, bits(a), bits(b), bits(c), bits(d)};
}

tokens make(uint32_t op, bool sat, std::initializer_list<tokens> ops)
{
    tokens t{0};
    for (const tokens &o : ops)
        t.insert(t.end(), o.begin(), o.end());
    t[0] = op | (sat ? 1u << 13 : 0u) | uint32_t(t.size()) << 24;
    return t;
}

struct edit
{
    size_t index;
    int kind;
    std::vector<tokens> code;
};

struct planner
{
    const program &pr;
    const light_values &v;
    light_support &s;
    std::vector<edit> &edits;

    const std::vector<instr> &ins() const
    {
        return pr.ins;
    }

    tokens copy(const operand &o) const
    {
        return tokens(pr.tokens.begin() + o.begin, pr.tokens.begin() + o.end);
    }

    tokens original(size_t i) const
    {
        return tokens(pr.tokens.begin() + ins()[i].at, pr.tokens.begin() + ins()[i].at + pr.sizes[i]);
    }

    // The micro-shadow's ao^power into t.x.
    std::vector<tokens> ao_power(uint32_t t, uint32_t a, char c) const
    {
        if (v.sun_power == 2.0f)
            return {make(OP_MUL, false, {dst(t, "x"), sel(a, c), sel(a, c)})};
        if (v.sun_power == 4.0f)
            return {make(OP_MUL, false, {dst(t, "x"), sel(a, c), sel(a, c)}),
                    make(OP_MUL, false, {dst(t, "x"), sel(t, 'x'), sel(t, 'x')})};
        return {make(OP_LOG, false, {dst(t, "x"), sel(a, c)}),
                make(OP_MUL, false, {dst(t, "x"), sel(t, 'x'), lit(v.sun_power)}),
                make(OP_EXP, false, {dst(t, "x"), sel(t, 'x')})};
    }

    // Where the final sun shadow multiplies the sun color, the register filled
    // from [1] of the sun's buffer. The AO and NdotL must be known there and the
    // shadow not read again before.
    bool final_sun(long shadow, long ndl, long ao_def, uint32_t a, char c, size_t &at, uint32_t &reg,
                   char &comp) const
    {
        const size_t n = ins().size();
        for (size_t i = 0; i < n; ++i)
        {
            const instr &in = ins()[i];
            if (!((in.op == OP_MUL || in.op == OP_MOV) && in.plain && !in.sat && in.ops.size() >= 2 &&
                  is_temp(in.ops[0])))
                continue;
            const operand &last = in.ops.back();
            if (last.type != T_CB || last.dims != 3 || !last.simple || last.ncomp != 4 || last.index[0] > 9 ||
                last.index[1] != 0 || last.index[2] != 1 || last.modifier > 1)
                continue;
            if (long(i) < shadow - 40)
                continue;
            const uint32_t sun = in.ops[0].index[0];
            for (size_t j = std::max<long>(long(i), shadow) + 1; j < n; ++j)
            {
                const instr &m = ins()[j];
                int hit = 0;
                if (arith(m, OP_MUL, 3) && one_word(m.ops[0]))
                {
                    const operand &x = m.ops[1], &y = m.ops[2];
                    if (is_temp(x) && x.modifier == 0 && four_same(comps(x)) && is_temp(y) && y.modifier == 0 &&
                        y.index[0] == sun)
                        hit = 1;
                    else if (is_temp(x) && x.modifier == 0 && x.index[0] == sun && is_temp(y) && y.modifier == 0 &&
                             four_same(comps(y)))
                        hit = 2;
                }
                if (hit != 0)
                {
                    if (long(j) <= ndl || ao_def < 0 || long(j) <= ao_def)
                        return false;
                    for (long w = ao_def + 1; w < long(j); ++w)
                        if (writes(ins()[w], a, c))
                            return false;
                    const operand &f = m.ops[hit];
                    at = j;
                    reg = f.index[0];
                    comp = comps(f)[0];
                    return true;
                }
                // The sun color register is overwritten. Scaling it in place is fine.
                if (!m.ops.empty() && is_temp(m.ops[0]) && m.ops[0].modifier == 0 && m.ops[0].index[0] == sun &&
                    mentions(m, sun) < 2)
                    break;
            }
        }
        return false;
    }

    // The component a destination component reads from a swizzle printed as fxc does (padded with its last letter).
    static char comp_at(const std::string &swizzle, char dest)
    {
        if (swizzle.empty())
            return 0;
        return swizzle[std::min<size_t>(comp_index(dest), swizzle.size() - 1)];
    }

    // The instruction's tokens with its plain temps renamed.
    tokens renamed(size_t i, uint32_t from_a, uint32_t to_a, uint32_t from_b, uint32_t to_b) const
    {
        tokens out = original(i);
        const size_t at = ins()[i].at;
        for (const operand &o : ins()[i].ops)
            if (o.type == T_TEMP && o.dims == 1 && o.simple)
            {
                uint32_t &reg = out[o.end - 1 - at];
                reg = reg == from_a ? to_a : reg == from_b ? to_b : reg;
            }
        return out;
    }

    // Code before instruction i. A second insert before the same instruction joins the first one's code (after it),
    // since light_patch refuses two separate inserts there.
    void before(size_t i, const std::vector<tokens> &code)
    {
        for (edit &e : edits)
            if (e.kind == BEFORE && e.index == i)
            {
                e.code.insert(e.code.end(), code.begin(), code.end());
                return;
            }
        edits.push_back({i, BEFORE, code});
    }

    // True when instruction b runs whenever instruction a ran before it in the same pass through the code: no
    // else, endif, endloop or case of a block that encloses a lies between them. Whole blocks between are fine.
    bool same_block(size_t a, size_t b) const
    {
        if (a >= b)
            return false;
        int depth = 0;
        for (size_t i = a + 1; i < b; ++i)
        {
            const uint32_t op = ins()[i].op;
            if (op == OP_IF || op == OP_LOOP || op == OP_SWITCH)
                ++depth;
            else if (op == OP_ENDIF || op == OP_ENDLOOP || op == OP_ENDSWITCH)
            {
                if (--depth < 0)
                    return false;
            }
            else if ((op == OP_ELSE || op == OP_CASE || op == OP_DEFAULT) && depth == 0)
                return false;
        }
        return depth == 0;
    }

    // The first instruction after the declarations (customdata, D3D10 and D3D11 declarations skipped).
    long first_code() const
    {
        for (size_t i = 0; i < ins().size(); ++i)
        {
            const uint32_t op = ins()[i].op;
            if (op != OP_CUSTOMDATA && !(op >= 88 && op <= 106) && !(op >= 143 && op <= 163))
                return long(i);
        }
        return -1;
    }

    // The indirect light site: the ambient mul, mul X.<3>, A, B with plain temp sources, followed by the CB1[1][8].z gain
    // mad, or by the luminance dp3 L, X, l(0.2126, 0.7152, 0.0722, 0) and then that mad. Exactly one, else -1 and why.
    long ambient_site(const char **why) const
    {
        const size_t n = ins().size();
        auto plain_temp = [](const operand &o) { return is_temp(o) && o.modifier == 0; };
        auto ambient_mul = [&](size_t i) {
            const instr &m = ins()[i];
            return arith(m, OP_MUL, 3) && is_temp(m.ops[0]) && m.ops[0].mode == 0 && m.ops[0].modifier == 0 &&
                   comps(m.ops[0]).size() == 3 && plain_temp(m.ops[1]) && plain_temp(m.ops[2]);
        };
        auto gain_mad = [&](size_t i) {
            const instr &m = ins()[i];
            if (!arith(m, OP_MAD, 4) || !is_temp(m.ops[0]) || m.ops[0].mode != 0 || comps(m.ops[0]).size() != 1)
                return false;
            const operand &c = m.ops[2];
            return cb_is(c, 1, 1, 8) && c.modifier == 0 && comp_at(comps(c), comps(m.ops[0])[0]) == 'z';
        };
        auto luminance = [&](size_t i, uint32_t x) {
            const instr &d = ins()[i];
            if (d.op != OP_DP3 || !d.plain || d.sat || d.ops.size() != 3 || !is_temp(d.ops[0]) ||
                d.ops[0].mode != 0 || comps(d.ops[0]).size() != 1 || !plain_temp(d.ops[1]) || d.ops[1].index[0] != x)
                return false;
            const operand &l = d.ops[2];
            if (l.type != T_IMM32 || l.ncomp != 4 || l.modifier != 0)
                return false;
            const uint32_t *v = pr.tokens.data() + l.begin + 1;
            return v[0] == bits(0.2126f) && v[1] == bits(0.7152f) && v[2] == bits(0.0722f) && v[3] == 0;
        };
        std::vector<size_t> sites;
        for (size_t i = 0; i + 1 < n; ++i)
        {
            if (!ambient_mul(i))
                continue;
            const uint32_t x = ins()[i].ops[0].index[0];
            if (gain_mad(i + 1) || (i + 2 < n && luminance(i + 1, x) && gain_mad(i + 2)))
                sites.push_back(i);
        }
        if (sites.size() != 1)
        {
            *why = sites.empty() ? "ambient multiply not found" : "ambient multiply found more than once";
            return -1;
        }
        return long(sites[0]);
    }

    // The combine: the game's probe bounce term, copied where the game adds it to the ambient. Sun form: the add that
    // reads the bounce factor r.c (after CB0[0][103].z and our Bounce light scale) and writes one operand of the
    // ambient mul: n = its two multiplicands. No-sun form: the mad with CB0[0][103].zzzz writing that operand:
    // n = bounce x CB0[0][103].z x k. n goes into the temp pr.temps + 2, zeroed at the start (paths without bounce
    // light keep 0). Anything not found leaves the combine out of this variant.
    void bounce_copy(long sun_site, const std::vector<size_t> &no_sun_sites)
    {
        const char *why = nullptr;
        const long at = ambient_site(&why);
        s.bounce_missing = why;
        if (at < 0)
            return;
        const instr &site = ins()[size_t(at)];
        const uint32_t ra = site.ops[1].index[0], rb = site.ops[2].index[0];
        s.bounce_missing = "both ambient operands in one register";
        if (ra == rb)
            return;
        const uint32_t nb = pr.temps + 2;
        struct found
        {
            size_t i;
            uint32_t d;
        };
        std::vector<found> adds;
        if (sun_site >= 0)
        {
            const operand &f = ins()[size_t(sun_site)].ops[0];
            const uint32_t r = f.index[0];
            const char c = comps(f)[0];
            for (size_t i = size_t(sun_site) + 1; i < size_t(at); ++i)
            {
                const instr &m = ins()[i];
                if (!arith(m, OP_MAD, 4) || m.sat || !is_temp(m.ops[0]) || m.ops[0].mode != 0 ||
                    comps(m.ops[0]).size() != 3)
                    continue;
                bool reads = false;
                for (int k = 1; k <= 2; ++k)
                {
                    const operand &o = m.ops[k];
                    reads |= is_temp(o) && o.index[0] == r && o.modifier == 0 && four_same(comps(o)) && comps(o)[0] == c;
                }
                const uint32_t d = m.ops[0].index[0];
                if (!reads || (d != ra && d != rb))
                    continue;
                bool rewritten = false;
                for (size_t j = size_t(sun_site) + 1; j < i && !rewritten; ++j)
                    rewritten = writes(ins()[j], r, c);
                if (!rewritten)
                    adds.push_back({i, d});
            }
        }
        else
            for (size_t i : no_sun_sites)
            {
                const uint32_t d = ins()[i].ops[0].index[0];
                if (i < size_t(at) && (d == ra || d == rb))
                    adds.push_back({i, d});
            }
        s.bounce_missing = "no bounce add writes an operand of the ambient multiply";
        if (adds.empty())
            return;
        s.bounce_missing = "bounce adds write different registers";
        for (const found &a : adds)
            if (a.d != adds[0].d)
                return;
        const uint32_t d = adds[0].d;
        s.bounce_missing = "no code before the bounce add";
        const long start = first_code();
        if (start < 0 || size_t(start) > adds[0].i)
            return;
        s.bounce_missing = nullptr;
        before(size_t(start), {make(OP_MOV, false, {dst(nb, "xyzw"), lit4(0.0f, 0.0f, 0.0f, 0.0f)})});
        for (const found &a : adds)
        {
            const instr &m = ins()[a.i];
            const std::string mask = comps(m.ops[0]);
            std::vector<tokens> code = {make(OP_MUL, false, {dst(nb, mask), copy(m.ops[1]), copy(m.ops[2])})};
            if (sun_site < 0 && v.bounce != 1.0f)
                code.push_back(make(OP_MUL, false, {dst(nb, mask), swz(nb, "xyzw"), lit(v.bounce)}));
            before(a.i, code);
        }
        // Later writes to the ambient's channels. The known one (all 541 variants that have one) is a lerp toward
        // another ambient source: add T, -D, Y then mad D, W.cccc, T, D, so D' = (1 - w) D + w Y and the bounce part
        // becomes (1 - w) n: n' = n - w n, put in before that mad. Any other write is flagged (bounce_rewritten).
        const std::string add_mask = comps(ins()[adds.back().i].ops[0]);
        for (size_t j = adds.back().i + 1; j < size_t(at); ++j)
        {
            const instr &m = ins()[j];
            if (m.ops.empty() || !is_temp(m.ops[0]) || m.ops[0].index[0] != d)
                continue;
            const std::string mask = comps(m.ops[0]);
            if (mask.find_first_of(add_mask) == std::string::npos)
                continue;
            bool lerp = arith(m, OP_MAD, 4) && !m.sat && m.ops[0].mode == 0 && mask == add_mask && is_temp(m.ops[1]) &&
                        m.ops[1].modifier == 0 && four_same(comps(m.ops[1])) && m.ops[1].index[0] != d &&
                        is_temp(m.ops[2]) && m.ops[2].modifier == 0 && is_temp(m.ops[3]) && m.ops[3].modifier == 0 &&
                        m.ops[3].index[0] == d;
            if (lerp)
            {
                // T's last writer before the mad: add T, -D, Y or add T, Y, -D.
                const uint32_t tr = m.ops[2].index[0];
                long w_at = -1;
                for (long k = long(j) - 1; k > long(adds.back().i) && w_at < 0; --k)
                    if (!ins()[size_t(k)].ops.empty() && is_temp(ins()[size_t(k)].ops[0]) &&
                        ins()[size_t(k)].ops[0].index[0] == tr)
                        w_at = k;
                const instr *a = w_at >= 0 ? &ins()[size_t(w_at)] : nullptr;
                lerp = a != nullptr && arith(*a, OP_ADD, 3) &&
                       ((is_temp(a->ops[1]) && a->ops[1].index[0] == d && a->ops[1].modifier == 1) ||
                        (is_temp(a->ops[2]) && a->ops[2].index[0] == d && a->ops[2].modifier == 1));
            }
            if (!lerp)
            {
                s.bounce_rewritten = true;
                continue;
            }
            tokens n_src = copy(m.ops[3]);
            n_src.back() = nb;
            before(j, {make(OP_MAD, false, {dst(nb, mask), swz_neg(m.ops[1].index[0], comps(m.ops[1])), n_src, n_src})});
        }
        // Right before the site, laid out like X (54 variants write X over the occlusion register, so this can't
        // wait until after the site): q = pr.temps + 3 gets occlusion x n (n_occ, the bounce part of X), n itself
        // becomes n_raw in X's channels.
        const int amb = ra == d ? 1 : 2;
        tokens amb_nb = copy(site.ops[amb]);
        amb_nb.back() = nb;
        const std::string xm = comps(site.ops[0]);
        // The occlusion's min: min O, M, A.cccc (A the screen AO, M the multi-bounce material AO; 5859 line 1092),
        // as the occlusion operand's last writer, or feeding it through movc O.xyzw, C, K x minv, minv (the class
        // factor form). M goes to mb = pr.temps + 4 and A to every channel of aa = pr.temps + 5 before that min, then
        // M is laid out like X. Not found: combine 3 adds ours only.
        const uint32_t mb = pr.temps + 4, aa = pr.temps + 5, occ_reg = site.ops[3 - amb].index[0];
        auto last_writer = [&](uint32_t reg, long before_i) {
            for (long j = before_i - 1; j >= 0; --j)
                if (!ins()[size_t(j)].ops.empty() && is_temp(ins()[size_t(j)].ops[0]) &&
                    ins()[size_t(j)].ops[0].index[0] == reg)
                    return j;
            return -1L;
        };
        auto min_form = [&](long j, int &m_op, int &a_op) {
            const instr &m = ins()[size_t(j)];
            if (!arith(m, OP_MIN_, 3) || m.sat || m.ops[0].mode != 0 || !is_temp(m.ops[1]) || !is_temp(m.ops[2]) ||
                m.ops[1].modifier != 0 || m.ops[2].modifier != 0)
                return false;
            const bool s1 = four_same(comps(m.ops[1])), s2 = four_same(comps(m.ops[2]));
            if (s1 == s2)
                return false;
            m_op = s1 ? 2 : 1;
            a_op = s1 ? 1 : 2;
            return true;
        };
        std::vector<tokens> pre = {make(OP_MUL, false, {dst(pr.temps + 3, xm), copy(site.ops[3 - amb]), amb_nb}),
                                   make(OP_MOV, false, {dst(nb, xm), amb_nb})};
        bool material = false;
        int m_op = 0, a_op = 0;
        long jmin = -1;
        const long jw = last_writer(occ_reg, at);
        if (jw >= 0 && min_form(jw, m_op, a_op))
            jmin = jw;
        else if (jw >= 0)
        {
            const instr &c = ins()[size_t(jw)];
            if (c.op == OP_MOVC && c.plain && !c.sat && c.ops.size() == 4 && comps(c.ops[0]) == "xyzw")
                for (int k = 2; k <= 3 && jmin < 0; ++k)
                    if (is_temp(c.ops[k]) && c.ops[k].modifier == 0 && comps(c.ops[k]) == "xyzw")
                    {
                        const long jj = last_writer(c.ops[k].index[0], jw);
                        if (jj >= 0 && min_form(jj, m_op, a_op))
                            jmin = jj;
                    }
        }
        if (jmin >= 0 && same_block(size_t(jmin), size_t(at)))
        {
            const instr &m = ins()[size_t(jmin)];
            before(size_t(jmin), {make(OP_MOV, false, {dst(mb, comps(m.ops[0])), copy(m.ops[size_t(m_op)])}),
                                  make(OP_MOV, false, {dst(aa, "xyzw"), copy(m.ops[size_t(a_op)])})});
            tokens mb_op = copy(site.ops[3 - amb]);
            mb_op.back() = mb;
            pre.push_back(make(OP_MOV, false, {dst(mb, xm), mb_op}));
            material = true;
        }
        s.material_min_in = material;
        before(size_t(at), pre);
        s.bounce_copy_in = true;
        s.bounce_reg = d;
    }

    // Surface reflections. Right after the reflection's CB2[0][88].w scale: X += c (S / E - X) with (S, c) read from
    // our texture at space-2 t<reg> and E the shader's own exposure. Two new temps: pr.temps and pr.temps + 1.
    const char *ssr2(uint32_t reg)
    {
        const size_t n = ins().size();
        const uint32_t t = pr.temps, u = pr.temps + 1;
        if (pr.srvs_unknown)
            return "a resource declaration of an unknown form";
        // A forward shader of the family (an interpolated input besides the position) draws geometry: our texture
        // describes the surface behind it.
        for (size_t i = 0; i < n; ++i)
        {
            const instr &in = ins()[i];
            if (in.op < 98 || in.op > 100 || pr.sizes[i] < 3)
                continue;
            const uint32_t *dt = pr.tokens.data() + in.at;
            const uint32_t mode = dt[0] >> 11 & 0xF;
            if (mode >= 2 && (dt[1] >> 12 & 0xFF) == 1 && (dt[1] >> 31) == 0 && dt[2] >= 1)
                return "a forward shader (interpolated inputs)";
        }

        // mul X.m3, X.any, CB2[0][88].wwww: exactly one.
        std::vector<size_t> muls;
        for (size_t i = 0; i < n; ++i)
        {
            const instr &in = ins()[i];
            if (arith(in, OP_MUL, 3) && is_temp(in.ops[0]) && in.ops[0].mode == 0 && in.ops[0].modifier == 0 &&
                is_temp(in.ops[1]) && in.ops[1].modifier == 0 && in.ops[1].index[0] == in.ops[0].index[0] &&
                cb_is(in.ops[2], 2, 0, 88) && in.ops[2].modifier == 0 && in.ops[2].mode == 1 &&
                comps(in.ops[2]) == "wwww")
                muls.push_back(i);
        }
        if (muls.size() > 1)
            return "reflection scale found more than once";
        auto plain_temp = [](const operand &o) { return is_temp(o) && o.modifier == 0; };
        auto is_scale = [](const operand &o) { return cb_is(o, 2, 0, 88) && o.mode == 1 && comps(o) == "wwww"; };
        // The second form (variants that also blend the game's water SSR): mul Y, X, CB2[0][88].wwww into a new
        // register, and X read once more later as mad A, -X, CB2[0][88].wwww, B. The edit then goes on X before the
        // scale, so every read of X sees it.
        bool second = false;
        size_t mi = muls.empty() ? 0 : muls[0];
        uint32_t x = 0;
        std::string m3, y_mask, x_sw;
        if (muls.empty())
        {
            std::vector<size_t> uses, copies;
            for (size_t i = 0; i < n; ++i)
            {
                const instr &in = ins()[i];
                bool use = false;
                for (const operand &o : in.ops)
                    use |= is_scale(o);
                if (!use)
                    continue;
                uses.push_back(i);
                if (arith(in, OP_MUL, 3) && is_temp(in.ops[0]) && in.ops[0].mode == 0 && in.ops[0].modifier == 0 &&
                    plain_temp(in.ops[1]) && is_scale(in.ops[2]) && in.ops[2].modifier == 0)
                    copies.push_back(i);
            }
            if (uses.empty())
            {
                // No probe read at all (no cube texture sampled): no ambient specular, nothing to replace.
                std::vector<uint32_t> cubes;
                for (const program::srv &r : pr.srvs)
                {
                    const uint32_t dim = pr.tokens[ins()[r.index].at] >> 11 & 0x1F;
                    if (r.op == OP_DCL_RESOURCE && (dim == 9 || dim == 10))
                        cubes.push_back(r.id);
                }
                bool probe = false;
                for (size_t i = 0; i < n && !probe; ++i)
                    for (const operand &o : ins()[i].ops)
                        probe |= o.type == T_RESOURCE && o.dims == 2 &&
                                 std::find(cubes.begin(), cubes.end(), o.index[0]) != cubes.end();
                if (!probe)
                    return LIGHT_SSR2_NO_PROBE;
            }
            if (copies.size() != 1)
                return copies.empty() ? "reflection scale not found" : "reflection scale copied more than once";
            mi = copies[0];
            x = ins()[mi].ops[1].index[0];
            y_mask = comps(ins()[mi].ops[0]);
            x_sw = comps(ins()[mi].ops[1]);
            if (uses.size() > 2)
                return "more than one other use of the reflection scale";
            for (size_t j : uses)
            {
                if (j == mi)
                    continue;
                const instr &o = ins()[j];
                if (j < mi || !arith(o, OP_MAD, 4) || !plain_temp(o.ops[0]) || o.ops[0].mode != 0 ||
                    !is_temp(o.ops[1]) || o.ops[1].modifier != 1 || o.ops[1].index[0] != x || !is_scale(o.ops[2]) ||
                    o.ops[2].modifier != 0 || !plain_temp(o.ops[3]))
                    return "the other use of the reflection scale is not the subtraction of the lerp";
            }
            if (mi < 2 || !arith(ins()[mi - 1], OP_MAD, 4) || !is_temp(ins()[mi - 1].ops[0]) ||
                ins()[mi - 1].ops[0].index[0] != x)
                return "mad not found";
            m3 = comps(ins()[mi - 1].ops[0]);
            second = true;
        }
        else
        {
            x = ins()[mi].ops[0].index[0];
            m3 = comps(ins()[mi].ops[0]);
        }
        if (mi < 2)
            return "mad not found";

        // mad X.m3, W.cccc, D.s, P.s: the lerp from the probe P.
        const instr &mad = ins()[mi - 1];
        if (!arith(mad, OP_MAD, 4) || !temp_is(mad.ops[0], x, m3) || mad.ops[0].mode != 0 ||
            !plain_temp(mad.ops[1]) || mad.ops[1].mode != 1 || !four_same(comps(mad.ops[1])) ||
            !plain_temp(mad.ops[2]) || !plain_temp(mad.ops[3]))
            return "mad not found";
        const uint32_t d = mad.ops[2].index[0], p = mad.ops[3].index[0];
        const std::string d_sw = comps(mad.ops[2]), p_sw = comps(mad.ops[3]);

        // add D.m1, -P.s, I.s or add D.m1, I.s, -P.s.
        const instr &add = ins()[mi - 2];
        if (!arith(add, OP_ADD, 3) || !is_temp(add.ops[0]) || add.ops[0].mode != 0 || add.ops[0].modifier != 0 ||
            add.ops[0].index[0] != d)
            return "add not found";
        const operand &a1 = add.ops[1], &a2 = add.ops[2];
        std::string ap_sw;
        if (is_temp(a1) && a1.modifier == 1 && a1.index[0] == p && plain_temp(a2))
            ap_sw = comps(a1);
        else if (plain_temp(a1) && is_temp(a2) && a2.modifier == 1 && a2.index[0] == p)
            ap_sw = comps(a2);
        else
            return "add not found";
        const std::string m1 = comps(add.ops[0]);
        if (m3.size() != 3 || m1.size() != 3)
            return "masks not 3 wide";
        for (char c : m3)
        {
            const char dk = comp_at(d_sw, c), pk = comp_at(p_sw, c);
            if (m1.find(dk) == std::string::npos || comp_at(ap_sw, dk) != pk)
                return "add and mad disagree on the reflection channels";
        }
        if (second)
        {
            if (y_mask.size() != 3)
                return "the scale does not read the lerp channels in order";
            for (size_t k = 0; k < 3; ++k)
                if (comp_at(x_sw, y_mask[k]) != m3[k])
                    return "the scale does not read the lerp channels in order";
        }

        // The new declaration: a copy of the first float4 texture2d declaration of space 2 with the next range id.
        uint32_t max_id = 0;
        long first = -1, last = -1;
        for (const program::srv &r : pr.srvs)
        {
            max_id = std::max(max_id, r.id);
            if (r.space == 2 && r.lower <= reg && reg <= r.upper)
                return "the register is declared already";
            if (r.space == 2 && r.lower <= LIGHT_SSIL_REGISTER && LIGHT_SSIL_REGISTER <= r.upper)
                return "the indirect light register is declared already";
            // dcl_resource_texture2d (float,float,float,float) in space 2.
            const uint32_t *dt = pr.tokens.data() + ins()[r.index].at;
            if (r.op == OP_DCL_RESOURCE && r.space == 2 && (dt[0] >> 11 & 0x1F) == 3 && dt[pr.sizes[r.index] - 2] == 0x5555)
            {
                if (first < 0)
                    first = long(r.index);
                last = long(r.index);
            }
        }
        if (pr.srvs.empty())
            return "no shader resource declarations";
        tokens dcl = {0x07001858u, 0x307e46u, 0, 0, 0, 0x5555u, 2}; // dcl_resource_texture2d (float x 4), space 2
        if (first >= 0)
            dcl = original(size_t(first));
        else
            // None of that space to copy: the same declaration from its tokens, after the last resource declaration.
            for (const program::srv &r : pr.srvs)
                last = std::max(last, long(r.index));
        if (dcl.size() != 7 || dcl[1] != 0x307e46 || dcl[6] != 2)
            return "no space-2 texture declaration to copy";
        dcl[2] = max_id + 1;
        dcl[3] = dcl[4] = reg;

        // The albedo read: the first sample_l rA.xyzw, rB.xyxx, T?[6].xyzw, S?[0], l(0.0) right after
        // mul r0.xy, v0.xyxx, CB2[0][52].xyxx.
        long sample = -1;
        for (size_t i = 1; i < n && sample < 0; ++i)
        {
            const instr &in = ins()[i];
            if (in.op != OP_SAMPLE_L || !in.plain || in.sat || in.ops.size() != 5)
                continue;
            const operand &dst_o = in.ops[0], &uv = in.ops[1], &res = in.ops[2], &smp = in.ops[3], &lod = in.ops[4];
            if (!temp_is(dst_o, dst_o.index[0], "xyzw") || dst_o.mode != 0 || !plain_temp(uv) || uv.mode != 1 ||
                comps(uv) != "xyxx" || res.type != T_RESOURCE || res.dims != 2 || !res.simple || res.modifier != 0 ||
                res.index[1] != 6 || res.mode != 1 || comps(res) != "xyzw" || smp.type != 6 || smp.dims != 2 ||
                !smp.simple || smp.modifier != 0 || smp.index[1] != 0 || lod.type != T_IMM32 || lod.ncomp != 1 ||
                lod.modifier != 0 || pr.tokens[lod.begin + 1] != 0)
                continue;
            const instr &m = ins()[i - 1];
            if (arith(m, OP_MUL, 3) && temp_is(m.ops[0], 0, "xy") && m.ops[0].mode == 0 && m.ops[1].type == 1 &&
                m.ops[1].dims == 1 && m.ops[1].simple && m.ops[1].index[0] == 0 && m.ops[1].modifier == 0 &&
                m.ops[1].mode == 1 && comps(m.ops[1]) == "xyxx" && cb_is(m.ops[2], 2, 0, 52) &&
                m.ops[2].modifier == 0 && m.ops[2].mode == 1 && comps(m.ops[2]) == "xyxx")
                sample = long(i);
        }
        if (sample < 0)
            return "albedo sample not found";
        const instr &si = ins()[sample];
        tokens read = renamed(size_t(sample), si.ops[0].index[0], t, si.ops[1].index[0], t);
        const size_t res_at = si.ops[2].begin - si.at;
        if ((read[res_at] & 0xFFFFF000u) != 0x00207000u)
            return "albedo sample has an unknown form";
        read[res_at + 1] = max_id + 1;
        read[res_at + 2] = reg;
        const tokens uv_line = renamed(size_t(sample - 1), 0, t, 0, t);

        // The exposure: the only structured space-1 t0 of stride 4, its first ld_structured rN.x, l(0), l(0), T[0].xxxx.
        long exp_id = -1;
        int exp_decls = 0;
        for (const program::srv &r : pr.srvs)
            if (r.op == OP_DCL_RESOURCE_STRUCTURED && r.lower == 0 && r.upper == 0 && r.stride == 4 && r.space == 1)
            {
                exp_id = long(r.id);
                ++exp_decls;
            }
        if (exp_decls != 1)
            return "exposure ld not found";
        long exposure = -1;
        for (size_t i = 0; i < n && exposure < 0; ++i)
        {
            const instr &in = ins()[i];
            if (in.op != OP_LD_STRUCTURED || !in.plain || in.sat || in.ops.size() != 4)
                continue;
            const operand &dst_o = in.ops[0], &res = in.ops[3];
            auto zero = [&](const operand &o) {
                return o.type == T_IMM32 && o.ncomp == 1 && o.modifier == 0 && pr.tokens[o.begin + 1] == 0;
            };
            if (is_temp(dst_o) && dst_o.modifier == 0 && comps(dst_o).size() == 1 && dst_o.mode == 0 &&
                zero(in.ops[1]) && zero(in.ops[2]) &&
                res.type == T_RESOURCE && res.dims == 2 && res.simple && res.modifier == 0 &&
                res.index[0] == uint32_t(exp_id) && res.index[1] == 0 && res.mode == 1 && comps(res) == "xxxx")
                exposure = long(i);
        }
        if (exposure < 0)
            return "exposure ld not found";
        const uint32_t e_reg = ins()[exposure].ops[0].index[0];
        // Written to u.x whatever component the shader loads into: the swizzle is .xxxx, so the value is the same.
        tokens e_line = renamed(size_t(exposure), e_reg, u, e_reg, u);
        uint32_t &e_mask = e_line[ins()[exposure].ops[0].begin - ins()[exposure].at];
        e_mask = (e_mask & ~0xF0u) | 0x10u;

        // The edit is on the probe P itself, before the lerp to the irradiance along R. Everything the game applies to
        // its probe then applies to ours the same way: the rough fade to irradiance, CB2[0][88].w, the env BRDF
        // (Fresnel, F0 from metalness), occlusion, cavity, near fade, fog, exposure. P' = P + c (S / E - P). Where
        // c = 0 the texture holds 0 and P' = P + 0 x (0 - P) = P bit for bit.
        auto swizzle_for = [](const std::string &mask, const char *src) {
            std::string s = "xxxx";
            for (size_t k = 0; k < mask.size(); ++k)
                s[comp_index(mask[k])] = src[k];
            return s;
        };
        // A source as fxc prints it: one letter is a select, four a swizzle.
        auto source = [](uint32_t reg, const std::string &c) { return c.size() == 1 ? sel(reg, c[0]) : swz(reg, c); };
        std::string pk, dk;
        for (char c : m3)
        {
            dk += comp_at(d_sw, c);
            pk += comp_at(p_sw, c);
        }
        const bool neg_first = is_temp(a1) && a1.modifier == 1 && a1.index[0] == p;
        const operand &i_op = neg_first ? a2 : a1;
        const tokens cb62 = {0x0030800Au, 2, 0, 62}; // CB2[0][62].x
        std::vector<tokens> code = {uv_line, read, e_line,
                                    make(OP_MUL, false, {dst(u, "x"), sel(u, 'x'), cb62}),
                                    make(OP_MAX, false, {dst(u, "x"), sel(u, 'x'), lit(1e-20f)}),
                                    make(OP_DIV, false, {dst(t, "xyz"), swz(t, "xyzx"), swz(u, "xxxx")}),
                                    make(OP_ADD, false, {dst(t, "xyz"), swz(t, "xyzx"), swz_neg(p, pk)}),
                                    make(OP_MAD, false, {dst(t, "xyz"), swz(t, "wwww"), swz(t, "xyzx"), swz(p, pk)})};
        // The game's final gloss: the one read by its probe mip line, add rM.c, -rG.c, CB0[0][211].x (exactly one).
        long mip_line = -1;
        int mip_lines = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const instr &in = ins()[i];
            if (arith(in, OP_ADD, 3) && is_temp(in.ops[0]) && in.ops[0].modifier == 0 &&
                comps(in.ops[0]).size() == 1 && is_temp(in.ops[1]) && in.ops[1].modifier == 1 &&
                in.ops[1].mode == 2 && cb_is(in.ops[2], 0, 0, 211) && in.ops[2].modifier == 0 &&
                in.ops[2].mode == 2 && comps(in.ops[2]) == "x")
            {
                mip_line = long(i);
                ++mip_lines;
            }
        }
        // The game's lerp, reading P' from t: channel k of t is rgb channel k.
        const tokens p_new = swz_neg(t, swizzle_for(dk, "xyz"));
        const tokens i_src = source(i_op.index[0], comps(i_op));
        code.push_back(neg_first ? make(OP_ADD, false, {dst(d, m1), p_new, i_src})
                                 : make(OP_ADD, false, {dst(d, m1), i_src, p_new}));
        const instr &lerp = ins()[mi - 1];
        edits.push_back({mi - 2, REPLACE, code});
        edits.push_back({mi - 1, REPLACE,
                         {make(OP_MAD, false, {dst(x, m3), swz(lerp.ops[1].index[0], comps(lerp.ops[1])),
                                               source(d, d_sw), swz(t, swizzle_for(m3, "xyz"))})}});
        std::vector<tokens> decls = {dcl};
        if (v.ssr2_gloss)
        {
            // After the gloss line: store (gloss, 1) at the pixel into our UAV (space-2 u0; no variant declares any UAV).
            if (mip_lines != 1)
                return "the reflection mip line (the final gloss) not found once";
            for (size_t i = 0; i < n; ++i)
                if (ins()[i].op >= 156 && ins()[i].op <= 158)
                    return "the shader declares a UAV already";
            const operand &g = ins()[mip_line].ops[1];
            const tokens v0 = {0x00101046u, 0}; // v0.xyxx
            edits.push_back({size_t(mip_line), AFTER,
                             {make(OP_FTOU, false, {dst(u, "xy"), v0}),
                              make(OP_MOV, false, {dst(u, "z"), sel(g.index[0], comps(g)[0])}),
                              make(OP_MOV, false, {dst(u, "w"), lit(1.0f)}),
                              make(OP_STORE_UAV_TYPED, false,
                                   {tokens{0x0021E0F2u, 0, LIGHT_SSR2_GLOSS_UAV}, swz(u, "xyyy"), swz(u, "zwww")})}});
            // dcl_uav_typed_texture2d (float,float,float,float) U0[0:0], space=2
            decls.push_back({0x0700189Cu, 0x0031EE46u, 0, LIGHT_SSR2_GLOSS_UAV, LIGHT_SSR2_GLOSS_UAV, 0x5555u, 2});
        }
        edits.push_back({size_t(last), AFTER, decls});
        return nullptr;
    }

    // Indirect lighting. Right after the ambient multiply (mul X.m3, occlusion, ambient irradiance; 5859 line 1108), before
    // the luminance dp3, the CB1[1][8] gains and the albedo multiply:
    //     X' = X - X sat(-a) + rgb / E,    (rgb, a) from our texture at space-2 t<reg>, E the shader's own exposure.
    // a >= 0 (our visibility) keeps the game's X. Where the texture holds 0 this is X - X x 0 + 0 / E = X. One new
    // temp (pr.temps), the one the AO edits share; it is free at this line. With the surface reflections in the same
    // copy our declaration takes a range id after theirs.
    const char *ssil(uint32_t reg, bool after_ssr2)
    {
        const size_t n = ins().size();
        const uint32_t t = pr.temps;
        if (pr.srvs_unknown)
            return "a resource declaration of an unknown form";
        for (size_t i = 0; i < n; ++i)
        {
            const instr &in = ins()[i];
            if (in.op < 98 || in.op > 100 || pr.sizes[i] < 3)
                continue;
            const uint32_t *dt = pr.tokens.data() + in.at;
            const uint32_t mode = dt[0] >> 11 & 0xF;
            if (mode >= 2 && (dt[1] >> 12 & 0xFF) == 1 && (dt[1] >> 31) == 0 && dt[2] >= 1)
                return "a forward shader (interpolated inputs)";
        }
        auto plain_temp = [](const operand &o) { return is_temp(o) && o.modifier == 0; };
        const char *site_missing = nullptr;
        const long site_at = ambient_site(&site_missing);
        if (site_at < 0)
            return site_missing;
        const size_t at = size_t(site_at);
        const uint32_t x = ins()[at].ops[0].index[0];
        const std::string m3 = comps(ins()[at].ops[0]);

        // The declaration, as the surface reflections make theirs (a copy of the first float4 texture2d of space 2).
        uint32_t max_id = 0;
        long first = -1, last = -1;
        for (const program::srv &r : pr.srvs)
        {
            max_id = std::max(max_id, r.id);
            if (r.space == 2 && r.lower <= reg && reg <= r.upper)
                return "the register is declared already";
            const uint32_t *dt = pr.tokens.data() + ins()[r.index].at;
            if (r.op == OP_DCL_RESOURCE && r.space == 2 && (dt[0] >> 11 & 0x1F) == 3 && dt[pr.sizes[r.index] - 2] == 0x5555)
            {
                if (first < 0)
                    first = long(r.index);
                last = long(r.index);
            }
        }
        if (pr.srvs.empty())
            return "no shader resource declarations";
        tokens dcl = {0x07001858u, 0x307e46u, 0, 0, 0, 0x5555u, 2};
        if (first >= 0)
            dcl = original(size_t(first));
        else
            for (const program::srv &r : pr.srvs)
                last = std::max(last, long(r.index));
        if (dcl.size() != 7 || dcl[1] != 0x307e46 || dcl[6] != 2)
            return "no space-2 texture declaration to copy";
        // The surface reflections take max + 1. Ours stays clear of max + 2 as well.
        const uint32_t id = max_id + 1 + (after_ssr2 ? 2 : 0);
        dcl[2] = id;
        dcl[3] = dcl[4] = reg;

        // The albedo read and its uv line, as the surface reflections find them.
        long sample = -1;
        for (size_t i = 1; i < n && sample < 0; ++i)
        {
            const instr &in = ins()[i];
            if (in.op != OP_SAMPLE_L || !in.plain || in.sat || in.ops.size() != 5)
                continue;
            const operand &dst_o = in.ops[0], &uv = in.ops[1], &res = in.ops[2], &smp = in.ops[3], &lod = in.ops[4];
            if (!temp_is(dst_o, dst_o.index[0], "xyzw") || dst_o.mode != 0 || !plain_temp(uv) || uv.mode != 1 ||
                comps(uv) != "xyxx" || res.type != T_RESOURCE || res.dims != 2 || !res.simple || res.modifier != 0 ||
                res.index[1] != 6 || res.mode != 1 || comps(res) != "xyzw" || smp.type != 6 || smp.dims != 2 ||
                !smp.simple || smp.modifier != 0 || smp.index[1] != 0 || lod.type != T_IMM32 || lod.ncomp != 1 ||
                lod.modifier != 0 || pr.tokens[lod.begin + 1] != 0)
                continue;
            const instr &m = ins()[i - 1];
            if (arith(m, OP_MUL, 3) && temp_is(m.ops[0], 0, "xy") && m.ops[0].mode == 0 && m.ops[1].type == 1 &&
                m.ops[1].dims == 1 && m.ops[1].simple && m.ops[1].index[0] == 0 && m.ops[1].modifier == 0 &&
                m.ops[1].mode == 1 && comps(m.ops[1]) == "xyxx" && cb_is(m.ops[2], 2, 0, 52) &&
                m.ops[2].modifier == 0 && m.ops[2].mode == 1 && comps(m.ops[2]) == "xyxx")
                sample = long(i);
        }
        if (sample < 0)
            return "albedo sample not found";
        const instr &si = ins()[sample];
        tokens read = renamed(size_t(sample), si.ops[0].index[0], t, si.ops[1].index[0], t);
        const size_t res_at = si.ops[2].begin - si.at;
        if ((read[res_at] & 0xFFFFF000u) != 0x00207000u)
            return "albedo sample has an unknown form";
        read[res_at + 1] = id;
        read[res_at + 2] = reg;
        const tokens uv_line = renamed(size_t(sample - 1), 0, t, 0, t);

        // The exposure ld, cloned to write t.w.
        long exp_id = -1;
        int exp_decls = 0;
        for (const program::srv &r : pr.srvs)
            if (r.op == OP_DCL_RESOURCE_STRUCTURED && r.lower == 0 && r.upper == 0 && r.stride == 4 && r.space == 1)
            {
                exp_id = long(r.id);
                ++exp_decls;
            }
        if (exp_decls != 1)
            return "exposure ld not found";
        long exposure = -1;
        for (size_t i = 0; i < n && exposure < 0; ++i)
        {
            const instr &in = ins()[i];
            if (in.op != OP_LD_STRUCTURED || !in.plain || in.sat || in.ops.size() != 4)
                continue;
            const operand &dst_o = in.ops[0], &res = in.ops[3];
            auto zero = [&](const operand &o) {
                return o.type == T_IMM32 && o.ncomp == 1 && o.modifier == 0 && pr.tokens[o.begin + 1] == 0;
            };
            if (is_temp(dst_o) && dst_o.modifier == 0 && comps(dst_o).size() == 1 && dst_o.mode == 0 &&
                zero(in.ops[1]) && zero(in.ops[2]) && res.type == T_RESOURCE && res.dims == 2 && res.simple &&
                res.modifier == 0 && res.index[0] == uint32_t(exp_id) && res.index[1] == 0 && res.mode == 1 &&
                comps(res) == "xxxx")
                exposure = long(i);
        }
        if (exposure < 0)
            return "exposure ld not found";
        const uint32_t e_reg = ins()[exposure].ops[0].index[0];
        tokens e_line = renamed(size_t(exposure), e_reg, t, e_reg, t);
        uint32_t &e_mask = e_line[ins()[exposure].ops[0].begin - ins()[exposure].at];
        e_mask = (e_mask & ~0xF0u) | 0x80u;

        std::string sw = "xxxx";
        for (size_t k = 0; k < m3.size(); ++k)
            sw[comp_index(m3[k])] = "xyz"[k];
        const tokens cb62 = {0x0030800Au, 2, 0, 62}; // CB2[0][62].x
        // The combine needs the texture's alpha (1 + V, or 0 where we have nothing) after the read.
        const bool keep_a = s.bounce_copy_in;
        std::vector<tokens> code = {uv_line, read};
        if (keep_a)
            code.push_back(make(OP_MOV, false, {dst(pr.temps + 1, "w"), sel(t, 'w')}));
        code.insert(code.end(), {make(OP_MOV, true, {dst(t, "w"), sel(t, 'w', 1)}),
                                    make(OP_MAD, false, {dst(x, m3), swz_neg(x, "xyzw"), swz(t, "wwww"), swz(x, "xyzw")}),
                                    e_line, make(OP_MUL, false, {dst(t, "w"), sel(t, 'w'), cb62}),
                                    make(OP_MAX, false, {dst(t, "w"), sel(t, 'w'), lit(1e-20f)}),
                                    make(OP_DIV, false, {dst(t, "xyz"), swz(t, "xyzx"), swz(t, "wwww")})});
        std::vector<tokens> decls = {dcl};
        // u = pr.temps + 1 is used by the combine (declared through bounce_copy_in then).
        const uint32_t u = pr.temps + 1;
        const uint32_t nb = pr.temps + 2, q = pr.temps + 3, mb = pr.temps + 4;
        const std::string xs = m3 + m3.substr(2);
        char f = 'x'; // the channel X's mask leaves free in nb, q and mb
        for (char k : std::string("xyzw"))
            if (m3.find(k) == std::string::npos)
                f = k;
        if (keep_a)
        {
            // n_raw in nb, n_occ in q, M in mb, all laid out like X (bounce_copy). u.w = a. valid = a >= 0.5 (we
            // have a value), V = sat(a - 1), c' = (1 - V) valid.
            code.push_back(make(OP_GE, false, {dst(u, "z"), sel(u, 'w'), lit(0.5f)}));
            code.push_back(make(OP_AND, false, {dst(u, "z"), sel(u, 'z'), lit(1.0f)}));
            code.push_back(make(OP_ADD, true, {dst(u, "w"), sel(u, 'w'), lit(-1.0f)}));
            code.push_back(make(OP_ADD, false, {dst(u, "w"), sel(u, 'w', 1), lit(1.0f)}));
            code.push_back(make(OP_MUL, false, {dst(u, "w"), sel(u, 'w'), sel(u, 'z')}));
            if (s.material_min_in)
            {
                // n_occ min(M, V) / min(M, A) - n_occ, times valid (V valid in nb.f; valid 0 makes it 0).
                const uint32_t aa = pr.temps + 5;
                code.push_back(make(OP_ADD, false, {dst(nb, std::string(1, f)), sel(u, 'z'), sel(u, 'w', 1)}));
                code.push_back(make(OP_MIN, false, {dst(aa, m3), swz(mb, "xyzw"), swz(aa, "xyzw")}));
                code.push_back(make(OP_MAX, false, {dst(aa, m3), swz(aa, "xyzw"), lit(1e-4f)}));
                code.push_back(make(OP_MIN, false, {dst(mb, m3), swz(mb, "xyzw"), swz(nb, std::string(4, f))}));
                code.push_back(make(OP_DIV, false, {dst(mb, m3), swz(mb, "xyzw"), swz(aa, "xyzw")}));
                code.push_back(make(OP_MAD, false, {dst(mb, m3), swz(mb, "xyzw"), swz(q, "xyzw"), swz_neg(q, "xyzw")}));
                code.push_back(make(OP_MAD, false, {dst(x, m3), swz(mb, "xyzw"), swz(u, "zzzz"), swz(x, "xyzw")}));
            }
        }
        code.push_back(make(OP_ADD, false, {dst(x, m3), swz(x, "xyzw"), swz(t, sw)}));
        gain_edits(at, x, m3, sw, t, code);
        edits.push_back({at, AFTER, code});
        edits.push_back({size_t(last), AFTER, decls});
        return nullptr;
    }

    // The ambient gain after the site (5859 lines 1110-1133): l = mad(a, CB1[1][8].z, b); if CB1[1][9].x < l a curve H
    // (G = 1), else G = max(exp(-0.005 / l) x rational(1 / l), 1) (H = 1). G scales the ambient diffuse, H everything.
    // Where the game's ambient is near 0, G reaches hundreds and turned ours into bright fringes. So ours is divided
    // by G x H after the endif, and the luminance dp3 after the site (o1.x) gets the game's own lum X back. Temps:
    // k = pr.temps + 6 (ours), f = pr.temps + 7. Pattern not found: the variant keeps ours as before.
    void gain_edits(size_t at, uint32_t x, const std::string &m3, const std::string &sw, uint32_t t,
                    std::vector<tokens> &code)
    {
        const size_t n = ins().size();
        const uint32_t k = pr.temps + 6, f = pr.temps + 7;
        size_t mad_at = at + 1;
        long dp3_at = -1;
        if (mad_at < n && ins()[mad_at].op == OP_DP3)
        {
            dp3_at = long(mad_at);
            ++mad_at;
        }
        auto scalar_temp = [&](const operand &o) { return is_temp(o) && o.mode == 0 && comps(o).size() == 1; };
        if (mad_at + 2 >= n || !arith(ins()[mad_at], OP_MAD, 4) || !scalar_temp(ins()[mad_at].ops[0]))
            return;
        const uint32_t gr = ins()[mad_at].ops[0].index[0];
        const char gc = comps(ins()[mad_at].ops[0])[0];
        const instr &lt = ins()[mad_at + 1];
        if (lt.op != OP_LT || lt.ops.size() != 3 || !scalar_temp(lt.ops[0]) || !cb_is(lt.ops[1], 1, 1, 9) ||
            comp_at(comps(lt.ops[1]), comps(lt.ops[0])[0]) != 'x' || !is_temp(lt.ops[2]) || lt.ops[2].index[0] != gr ||
            comp_at(comps(lt.ops[2]), comps(lt.ops[0])[0]) != gc)
            return;
        const uint32_t hr = lt.ops[0].index[0];
        const char hc = comps(lt.ops[0])[0];
        const size_t if_at = mad_at + 2;
        if (ins()[if_at].op != OP_IF || ins()[if_at].ops.size() != 1 || !is_temp(ins()[if_at].ops[0]) ||
            ins()[if_at].ops[0].index[0] != hr)
            return;
        long else_at = -1, end_at = -1;
        int depth = 0;
        for (size_t i = if_at + 1; i < n && end_at < 0; ++i)
        {
            const uint32_t op = ins()[i].op;
            if (op == OP_IF || op == OP_LOOP || op == OP_SWITCH)
                ++depth;
            else if (op == OP_ENDIF || op == OP_ENDLOOP || op == OP_ENDSWITCH)
            {
                if (depth == 0 && op == OP_ENDIF)
                    end_at = long(i);
                else
                    --depth;
            }
            else if (op == OP_ELSE && depth == 0)
                else_at = long(i);
        }
        if (else_at < 0 || end_at < 0)
            return;
        auto mov_one = [&](size_t i, uint32_t reg, char c) {
            const instr &m = ins()[i];
            return arith(m, OP_MOV, 2) && temp_is(m.ops[0], reg, std::string(1, c)) && m.ops[1].type == T_IMM32 &&
                   m.ops[1].ncomp == 1 && pr.tokens[m.ops[1].begin + 1] == bits(1.0f);
        };
        bool g_one = false, h_one = false;
        for (size_t i = if_at + 1; i < size_t(else_at); ++i)
            g_one |= mov_one(i, gr, gc);
        for (size_t i = size_t(else_at) + 1; i < size_t(end_at); ++i)
            h_one |= mov_one(i, hr, hc);
        if (!g_one || !h_one)
            return;
        for (size_t i = at + 1; i <= size_t(end_at); ++i)
            for (char c : m3)
                if (writes(ins()[i], x, c))
                    return;
        code.insert(code.begin(), make(OP_DP3, false, {dst(f, "y"), swz(x, m3 + m3.substr(2)),
                                                       lit4(0.2126f, 0.7152f, 0.0722f, 0.0f)}));
        code.push_back(make(OP_MOV, false, {dst(k, "xyzw"), swz(t, "xyzw")}));
        if (dp3_at >= 0)
        {
            const operand &d = ins()[size_t(dp3_at)].ops[0];
            edits.push_back({size_t(dp3_at), AFTER, {make(OP_MOV, false, {dst(d.index[0], comps(d)), sel(f, 'y')})}});
        }
        edits.push_back({size_t(end_at), AFTER,
                         {make(OP_MUL, false, {dst(f, "x"), sel(gr, gc), sel(hr, hc)}),
                          make(OP_MAX, false, {dst(f, "x"), sel(f, 'x'), lit(1e-6f)}),
                          make(OP_DIV, false, {dst(f, "x"), lit(1.0f), sel(f, 'x')}),
                          make(OP_ADD, false, {dst(f, "x"), sel(f, 'x'), lit(-1.0f)}),
                          make(OP_MAD, false, {dst(x, m3), swz(k, sw), swz(f, "xxxx"), swz(x, "xyzw")})}});
        s.gain_in = true;
    }

    const char *run()
    {
        const size_t n = ins().size();
        const uint32_t t = pr.temps; // one new temp

        // The AO: the ld from t3 of space 2, the component that reads channel z.
        long ld = -1;
        int lds = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const instr &in = ins()[i];
            if (in.op != OP_LD || in.ops.size() != 3)
                continue;
            const operand &r = in.ops[2];
            if (r.type != T_RESOURCE || r.dims != 2 || !r.simple || r.index[1] != 3)
                continue;
            bool space2 = false;
            for (const resource &res : pr.resources)
                space2 |= res.id == r.index[0] && res.lower == 3 && res.upper == 3 && res.space == 2;
            if (!space2)
                continue;
            if (lds++ == 0)
                ld = long(i);
        }
        if (lds == 0)
            return "no AO read";
        if (lds > 1)
            return "AO read found more than once";
        const instr &ldi = ins()[ld];
        const operand &ld_dst = ldi.ops[0], &ld_res = ldi.ops[2];
        if (!is_temp(ld_dst) || ld_dst.mode != 0 || ld_res.ncomp != 4 || ld_res.mode != 1)
            return "AO read has an unknown form";
        const std::string ld_mask = comps(ld_dst), ld_swz = comps(ld_res);
        uint32_t a = ld_dst.index[0];
        char c = 0;
        for (char ch : ld_mask)
            if (c == 0 && ld_swz[comp_index(ch)] == 'z')
                c = ch;
        if (c == 0)
            return "AO channel not read";
        // Family B lifts the AO toward 1 under puddles. The lifted value is what the lighting uses.
        for (size_t i = size_t(ld) + 1; i < n; ++i)
        {
            const instr &in = ins()[i];
            if (arith(in, OP_MAD, 4) && is_temp(in.ops[0]) && comps(in.ops[0]).size() == 1 &&
                one_word(in.ops[1]) && one_word(in.ops[2]) && temp_is(in.ops[3], a, std::string(1, c)))
            {
                a = in.ops[0].index[0];
                c = comps(in.ops[0])[0];
                break;
            }
            if (writes(in, a, c))
                break;
        }
        s.ao_register = a;
        s.ao_component = c;

        // min(material AO, AO): exactly one.
        std::vector<size_t> mins;
        for (size_t i = 0; i < n; ++i)
        {
            const instr &in = ins()[i];
            if (arith(in, OP_MIN, 3) && is_temp(in.ops[0]) && one_word(in.ops[1]) &&
                temp_is(in.ops[2], a, std::string(4, c)))
                mins.push_back(i);
        }
        s.min_sites = int(mins.size());
        if (mins.empty())
            return "min site not found";
        if (mins.size() > 1)
            return "min site found more than once";
        const size_t use = mins[0];

        if (v.blend != 0.0f)
        {
            const instr &m = ins()[use];
            const std::string mask = comps(m.ops[0]);
            const uint32_t d = m.ops[0].index[0];
            const float k = v.blend, rest = float(1.0 - double(v.blend));
            edits.push_back({use, REPLACE,
                             {make(OP_MAX, false, {dst(t, mask), copy(m.ops[1]), copy(m.ops[2])}), original(use),
                              make(OP_MAD, false, {dst(t, mask), swz(t, "xyzw"), lit4(k, k, k, k), lit4(rest, rest, rest, rest)}),
                              make(OP_MUL, false, {dst(d, mask), swz(d, "xyzw"), swz(t, "xyzw")})}});
        }

        // The sun shadow: min(.., CB0[0][99].x). NdotL: the first dp3 with [0] of the sun's buffer.
        long shadow = -1, ndl = -1;
        for (size_t i = 0; i < n && shadow < 0; ++i)
        {
            const instr &in = ins()[i];
            if (arith(in, OP_MIN, 3) && is_temp(in.ops[0]) && comps(in.ops[0]).size() == 1 &&
                temp_is(in.ops[1], in.ops[0].index[0], comps(in.ops[0])) && cb_is(in.ops[2], 0, 0, 99) &&
                in.ops[2].modifier == 0 && comps(in.ops[2]) == "x")
                shadow = long(i);
        }
        for (size_t i = 0; i < n && ndl < 0; ++i)
        {
            const instr &in = ins()[i];
            if (in.op == OP_DP3 && in.plain && in.ops.size() == 3 && is_temp(in.ops[0]) &&
                comps(in.ops[0]).size() == 1 && is_temp(in.ops[1]) && in.ops[1].modifier == 0 &&
                comps(in.ops[1]) == "xyzx" && in.ops[2].type == T_CB && in.ops[2].dims == 3 && in.ops[2].simple &&
                in.ops[2].index[0] <= 9 && in.ops[2].index[1] == 0 && in.ops[2].index[2] == 0 &&
                in.ops[2].modifier == 0 && comps(in.ops[2]) == "xyzx")
                ndl = long(i);
        }

        // Contact shadow: the mask's channel y, min'ed with the sun shadow right before the CB0[0][99].x min.
        if (shadow > 0)
        {
            std::string contact;
            for (char ch : ld_mask)
                if (ld_swz[comp_index(ch)] == 'y')
                    contact += ch;
            const uint32_t lr = ld_dst.index[0];
            const instr &m = ins()[shadow - 1];
            bool named = false, nested = false;
            if (!contact.empty() && m.op == OP_MIN && m.plain && !m.sat)
                for (const operand &o : m.ops)
                {
                    named |= is_temp(o) && o.index[0] == lr && comps(o)[0] == contact[0];
                    for (const ref &r : o.nested)
                        nested |= r.reg == lr && r.comps[0] == contact[0];
                }
            if (named && !nested)
            {
                s.contact = true;
                if (v.contact != 1.0f)
                {
                    // contact' = sat(1 + k (contact - 1)): 0 none, 1 the game's.
                    tokens line{0};
                    for (const operand &o : m.ops)
                    {
                        if (is_temp(o) && o.index[0] == lr && comps(o)[0] == contact[0])
                        {
                            std::string cs = comps(o);
                            cs[0] = 'w';
                            tokens r = o.mode == 0 ? dst(t, cs) : o.mode == 1 ? swz(t, cs) : sel(t, 'w');
                            if (o.modifier != 0)
                                r = o.mode == 2 ? sel(t, 'w', o.modifier)
                                                : tokens{r[0] | 0x80000000u, 1u | o.modifier << 6, t};
                            line.insert(line.end(), r.begin(), r.end());
                        }
                        else
                        {
                            const tokens r = copy(o);
                            line.insert(line.end(), r.begin(), r.end());
                        }
                    }
                    line[0] = OP_MIN | uint32_t(line.size()) << 24;
                    edits.push_back({size_t(shadow - 1), REPLACE,
                                     {make(OP_ADD, false, {dst(t, "w"), sel(lr, contact[0]), lit(-1.0f)}),
                                      make(OP_MAD, true, {dst(t, "w"), sel(t, 'w'), lit(v.contact), lit(1.0f)}),
                                      line}});
                }
            }
        }

        if (shadow >= 0 && ndl >= 0)
        {
            s.sun_light = true;
            const instr &si = ins()[shadow], &ni = ins()[ndl];
            const uint32_t rs = si.ops[0].index[0], rn = ni.ops[0].index[0];
            const char cs = comps(si.ops[0])[0], cn = comps(ni.ops[0])[0];
            // The AO is the last write of its register before the min site.
            long ao_def = -1;
            for (size_t j = 0; j < use; ++j)
                if (writes(ins()[j], a, c))
                    ao_def = long(j);
            const long at = std::max({shadow, ndl, ao_def >= 0 ? ao_def : 0L}) + 1;
            auto chain = [&](const instr &in) {
                const std::string one(1, cs);
                return arith(in, OP_MUL, 3) && temp_is(in.ops[0], rs, one) &&
                       ((temp_is(in.ops[2], rs, one) && one_word(in.ops[1])) ||
                        (temp_is(in.ops[1], rs, one) && one_word(in.ops[2])));
            };
            bool touched = false, ao_lost = false;
            for (long j = shadow + 1; j < at; ++j)
                touched |= touches(ins()[j], rs, cs) && !chain(ins()[j]);
            for (long j = ao_def + 1; ao_def >= 0 && j < at; ++j)
                ao_lost |= writes(ins()[j], a, c);
            // NdotL must still be in its register where it is read.
            auto ndl_kept = [&](long until) {
                for (long j = ndl + 1; j < until; ++j)
                    if (writes(ins()[j], rn, cn))
                        return false;
                return true;
            };
            size_t sun_at = 0;
            uint32_t s_reg = 0;
            char s_c = 0;
            if (ao_def < 0 || ao_def > long(use) || ao_lost)
                s.sun_missing = "AO register reused before the sun site";
            else if (touched && final_sun(shadow, ndl, ao_def, a, c, sun_at, s_reg, s_c))
            {
                if (ndl_kept(long(sun_at)))
                    s.sun_path = 2;
                else
                    s.sun_missing = "NdotL overwritten before the sun color";
            }
            else if (touched)
            {
                bool kept = ao_def < shadow;
                for (long j = ao_def + 1; kept && j < shadow; ++j)
                    kept = !writes(ins()[j], a, c);
                if (kept)
                    s.sun_path = 3;
                else
                    s.sun_missing = "sun shadow used before NdotL is known";
            }
            else if (ndl_kept(at))
                s.sun_path = 1;
            else
                s.sun_missing = "NdotL overwritten before the sun site";

            if (v.sun != 0.0f && s.sun_path != 0)
            {
                std::vector<tokens> seq = ao_power(t, a, c);
                if (s.sun_path == 3)
                {
                    seq.push_back(make(OP_MUL, true, {dst(t, "x"), sel(t, 'x'), lit(2.0f)}));
                    seq.push_back(make(OP_ADD, false, {dst(t, "x"), sel(t, 'x'), lit(-1.0f)}));
                    seq.push_back(make(OP_MAD, false, {dst(t, "x"), sel(t, 'x'), lit(v.sun), lit(1.0f)}));
                    seq.push_back(make(OP_MUL, false, {dst(rs, std::string(1, cs)), sel(rs, cs), sel(t, 'x')}));
                    edits.push_back({size_t(shadow), AFTER, seq});
                }
                else
                {
                    const uint32_t r = s.sun_path == 2 ? s_reg : rs;
                    const char rc = s.sun_path == 2 ? s_c : cs;
                    seq.push_back(make(OP_MAD, false, {dst(t, "x"), sel(t, 'x'), lit(2.0f), sel(rn, cn, 2)}));
                    seq.push_back(make(OP_ADD, true, {dst(t, "x"), sel(t, 'x'), lit(-1.0f)}));
                    seq.push_back(make(OP_ADD, false, {dst(t, "x"), sel(t, 'x'), lit(-1.0f)}));
                    seq.push_back(make(OP_MAD, false, {dst(t, "x"), sel(t, 'x'), lit(v.sun), lit(1.0f)}));
                    seq.push_back(make(OP_MUL, false, {dst(r, std::string(1, rc)), sel(r, rc), sel(t, 'x')}));
                    edits.push_back({s.sun_path == 2 ? sun_at : size_t(at), BEFORE, seq});
                }
            }
        }

        // Bounce light: mul r.c, r.c, CB0[0][103].z, exactly one.
        long bounce = -1;
        int bounces = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const instr &in = ins()[i];
            if (arith(in, OP_MUL, 3) && is_temp(in.ops[0]) && comps(in.ops[0]).size() == 1 &&
                temp_is(in.ops[1], in.ops[0].index[0], comps(in.ops[0])) && cb_is(in.ops[2], 0, 0, 103) &&
                in.ops[2].modifier == 0 && comps(in.ops[2]) == "z")
            {
                bounce = long(i);
                ++bounces;
            }
        }
        s.bounce = bounces == 1;
        if (s.bounce && v.bounce != 1.0f)
        {
            const operand &d = ins()[bounce].ops[0];
            const char dc = comps(d)[0];
            edits.push_back({size_t(bounce), AFTER,
                             {make(OP_MUL, false, {dst(d.index[0], std::string(1, dc)), sel(d.index[0], dc), lit(v.bounce)})}});
        }
        std::vector<size_t> no_sun;
        if (bounces == 0)
        {
            // Variants without the sun write it in one step: mad D, bounce, CB0[0][103].zzzz, sky (one or two
            // places, any three components). The sky is copied first. Some variants add into the sky's own
            // register. After the mad, D = k (D - sky) + sky scales only the bounce part.
            std::vector<size_t> sites;
            bool clean = true;
            for (size_t i = 0; i < n; ++i)
            {
                const instr &in = ins()[i];
                if (!arith(in, OP_MAD, 4) || !cb_is(in.ops[2], 0, 0, 103))
                    continue;
                const operand &d = in.ops[0], &sky = in.ops[3];
                if (is_temp(d) && comps(d).size() == 3 && is_temp(sky) && sky.modifier == 0 && sky.nested.empty() &&
                    in.ops[2].modifier == 0 && comps(in.ops[2])[0] == 'z')
                    sites.push_back(i);
                else
                    clean = false;
            }
            s.bounce = clean && !sites.empty();
            no_sun = sites;
            if (s.bounce && v.bounce != 1.0f)
                for (size_t i : sites)
                {
                    const instr &in = ins()[i];
                    const uint32_t d = in.ops[0].index[0];
                    const std::string mask = comps(in.ops[0]);
                    const float k = v.bounce;
                    edits.push_back({i, BEFORE,
                                     {make(OP_MOV, false, {dst(t, mask), swz(in.ops[3].index[0], comps(in.ops[3]))})}});
                    edits.push_back({i, AFTER,
                                     {make(OP_ADD, false, {dst(d, mask), swz(d, "xyzw"), swz_neg(t, "xyzw")}),
                                      make(OP_MAD, false, {dst(d, mask), swz(d, "xyzw"), lit4(k, k, k, k),
                                                           swz(t, "xyzw")})}});
                }
        }
        if (v.ssil != 0)
        {
            s.bounce_missing = "the bounce light site is not found";
            if (s.bounce)
                bounce_copy(bounces == 1 ? bounce : -1, no_sun);
        }

        // Sky light: the SH sky irradiance, finished by mad X.<3 comps>, CB1[1][6].<swizzle>, any, X.<swizzle>.
        // Every variant evaluates it, some up to three times. Its luminance tells day from night. Measured
        // 2026-11-21: night ~1e-6, dusk ~0.1, clear day 3..10. The scale blends from the night value to the
        // day value on a log scale: w = saturate(log2(L) / log2(1e4) + 1), night below 1e-4, day above 1.
        for (size_t i = 0; i < n; ++i)
        {
            const instr &in = ins()[i];
            if (!arith(in, OP_MAD, 4) || in.sat || !is_temp(in.ops[0]) || comps(in.ops[0]).size() != 3 ||
                !cb_is(in.ops[1], 1, 1, 6) || !is_temp(in.ops[3]) || in.ops[3].index[0] != in.ops[0].index[0] ||
                in.ops[3].modifier != 0 || in.ops[1].modifier != 0)
                continue;
            s.sky = true;
            if (v.sky == 1.0f && v.sky_night == 1.0f)
                continue;
            const uint32_t x = in.ops[0].index[0];
            const std::string mask = comps(in.ops[0]);
            const float a = float(1.0 / std::log2(1e4)), range = v.sky - v.sky_night;
            edits.push_back({i, AFTER,
                             {make(OP_DP3, false, {dst(t, "x"), swz(x, mask), lit4(0.2126f, 0.7152f, 0.0722f, 0.0f)}),
                              make(OP_MAX, false, {dst(t, "x"), sel(t, 'x'), lit(1e-30f)}),
                              make(OP_LOG, false, {dst(t, "x"), sel(t, 'x')}),
                              make(OP_MAD, true, {dst(t, "x"), sel(t, 'x'), lit(a), lit(1.0f)}),
                              make(OP_MAD, false, {dst(t, "x"), sel(t, 'x'), lit(range), lit(v.sky_night)}),
                              make(OP_MUL, false, {dst(x, mask), swz(x, "xyzw"), swz(t, "xxxx")})}});
        }
        return nullptr;
    }
};

const char *plan(const program &pr, const light_values &v, light_support &s, std::vector<edit> &edits)
{
    planner p{pr, v, s, edits};
    const char *failed = p.run();
    // The surface reflections do not need the AO sites. A variant the AO edits can't take gets our reflection alone.
    if (failed != nullptr)
    {
        edits.clear();
        s.bounce_copy_in = false;
        if (v.ssil != 0)
            s.bounce_missing = failed; // the AO edits can't go in: the copy has none of their sites
    }
    std::vector<edit> spare;
    const size_t before_ssr2 = edits.size();
    planner q{pr, v, s, v.ssr2 != 0 ? edits : spare};
    s.ssr2_missing = q.ssr2(v.ssr2 != 0 ? v.ssr2 : LIGHT_SSR2_REGISTER);
    if (v.ssr2 != 0 && s.ssr2_missing != nullptr)
    {
        // A failed check may have planned part of it already.
        edits.resize(before_ssr2);
        if (v.ssil == 0)
            return s.ssr2_missing;
    }
    s.ssr2_in = v.ssr2 != 0 && s.ssr2_missing == nullptr;
    // Indirect light goes in on its own where the reflections can't (no probe reflection, other forms): the ambient site is in all 541.
    std::vector<edit> spare2;
    const size_t before_ssil = edits.size();
    planner r{pr, v, s, v.ssil != 0 ? edits : spare2};
    s.ssil_missing = r.ssil(v.ssil != 0 ? v.ssil : LIGHT_SSIL_REGISTER, s.ssr2_in);
    if (v.ssil != 0 && s.ssil_missing != nullptr)
    {
        edits.resize(before_ssil);
        if (!s.ssr2_in)
            return s.ssil_missing;
    }
    s.ssil_in = v.ssil != 0 && s.ssil_missing == nullptr;
    s.gain_in = s.gain_in && s.ssil_in;
    if (s.ssr2_in || s.ssil_in)
        return nullptr;
    return failed;
}
} // namespace

bool light_is_candidate(const uint8_t *code, size_t size)
{
    if (code == nullptr)
        return false;
    bool found[2] = {};
    for (size_t at = 0; at + 4 <= size; at += 4)
    {
        const uint32_t t = read32(code, at);
        found[0] |= t == HALLMARK[0];
        found[1] |= t == HALLMARK[1];
    }
    return found[0] && found[1];
}

bool light_changes(const light_values &v)
{
    if (v.ssr2 != 0 || v.ssil != 0)
        return true;
    return v.blend != 0.0f || v.sun != 0.0f || v.bounce != 1.0f || v.sky != 1.0f || v.sky_night != 1.0f ||
           v.contact != 1.0f;
}

light_support light_check(const uint8_t *code, size_t size)
{
    light_support s;
    program pr;
    if (!light_is_candidate(code, size))
        s.failed = "hallmark literals not found";
    else if (const char *failed = parse(code, size, pr))
        s.failed = failed;
    else
    {
        std::vector<edit> edits;
        s.failed = plan(pr, light_values{}, s, edits);
    }
    return s;
}

light_support light_check_with(const uint8_t *code, size_t size, const light_values &v)
{
    light_support s;
    program pr;
    if (!light_is_candidate(code, size))
        s.failed = "hallmark literals not found";
    else if (const char *failed = parse(code, size, pr))
        s.failed = failed;
    else
    {
        std::vector<edit> edits;
        s.failed = plan(pr, v, s, edits);
    }
    return s;
}

bool light_patch(const uint8_t *code, size_t size, const light_values &v, std::vector<uint8_t> &out)
{
    program pr;
    light_support s;
    std::vector<edit> edits;
    if (!light_is_candidate(code, size) || parse(code, size, pr) != nullptr || plan(pr, v, s, edits) != nullptr)
        return false;
    // Two inserts before one instruction would share the one extra register in an order that is not checked.
    // None of the 528 known variants has that. An unknown one is left alone.
    for (size_t a = 0; a < edits.size(); ++a)
        for (size_t b = a + 1; b < edits.size(); ++b)
            if (edits[a].kind == BEFORE && edits[b].kind == BEFORE && edits[a].index == edits[b].index)
                return false;

    std::vector<tokens> list(pr.ins.size());
    for (size_t i = 0; i < pr.ins.size(); ++i)
        list[i].assign(pr.tokens.begin() + pr.ins[i].at, pr.tokens.begin() + pr.ins[i].at + pr.sizes[i]);
    if (!edits.empty())
        list[pr.temps_at][1] = pr.temps + 1;
    if (s.ssr2_in)
        list[pr.temps_at][1] = pr.temps + 2; // the surface reflections use a second temp
    if (s.bounce_copy_in)
        list[pr.temps_at][1] = pr.temps + 6; // the bounce copy uses four more
    if (s.gain_in)
        list[pr.temps_at][1] = pr.temps + 8; // the gain edits use two more
    // Back to front. Two edits at one index: the replacement first, then what goes before it.
    std::stable_sort(edits.begin(), edits.end(), [](const edit &x, const edit &y) {
        return x.index != y.index ? x.index > y.index : x.kind < y.kind;
    });
    for (const edit &e : edits)
    {
        const auto at = list.begin() + e.index;
        if (e.kind == REPLACE)
            list.insert(list.erase(at), e.code.begin(), e.code.end());
        else
            list.insert(e.kind == AFTER ? at + 1 : at, e.code.begin(), e.code.end());
    }
    tokens stream;
    for (const tokens &ins : list)
        stream.insert(stream.end(), ins.begin(), ins.end());
    if (stream.size() + 2 > 0x0FFFFFFF)
        return false;

    const size_t old_end = pr.stream + size_t(pr.length) * 4;
    out.assign(code, code + pr.stream + 8);
    out.insert(out.end(), reinterpret_cast<const uint8_t *>(stream.data()),
               reinterpret_cast<const uint8_t *>(stream.data() + stream.size()));
    out.insert(out.end(), code + old_end, code + size);
    const int64_t delta = int64_t(out.size()) - int64_t(size);
    auto write32 = [&](size_t at, uint32_t value) { memcpy(out.data() + at, &value, 4); };
    write32(pr.stream + 4, uint32_t(stream.size() + 2));
    write32(pr.chunk + 4, uint32_t(int64_t(read32(code, pr.chunk + 4)) + delta));
    write32(24, uint32_t(out.size()));
    const uint32_t chunks = read32(code, 28);
    for (uint32_t i = 0; i < chunks; ++i)
    {
        const size_t slot = 32 + size_t(i) * 4;
        const uint32_t at = read32(code, slot);
        if (at > pr.chunk)
            write32(slot, uint32_t(int64_t(at) + delta));
    }
    uint32_t sum[4];
    dxbc_checksum(out.data(), out.size(), sum);
    memcpy(out.data() + 4, sum, 16);
    return true;
}
