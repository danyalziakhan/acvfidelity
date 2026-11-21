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
    return p.run();
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
