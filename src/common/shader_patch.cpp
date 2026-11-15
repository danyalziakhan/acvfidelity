#include "shader_patch.h"

#include <cstring>

namespace
{
// ---------------------------------------------------------------- MD5

uint32_t rotate(uint32_t x, int c)
{
    return (x << c) | (x >> (32 - c));
}

void md5_block(uint32_t state[4], const uint8_t block[64])
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    memcpy(m, block, 64);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; ++i)
    {
        uint32_t f;
        int g;
        if (i < 16)
        {
            f = (b & c) | (~b & d);
            g = i;
        }
        else if (i < 32)
        {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        }
        else if (i < 48)
        {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        }
        else
        {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const uint32_t next = d;
        d = c;
        c = b;
        b = b + rotate(a + f + K[i] + m[g], R[i]);
        a = next;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

} // namespace

void dxbc_checksum(const uint8_t *data, size_t size, uint32_t out[4])
{
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const uint8_t *p = data + 20;
    const uint32_t length = uint32_t(size - 20);
    const uint32_t bits = length * 8;
    const uint32_t bits_tail = (bits >> 2) | 1;
    const uint32_t full = length / 64 * 64;
    for (uint32_t i = 0; i < full; i += 64)
        md5_block(state, p + i);
    const uint32_t rest = length - full;
    uint8_t last[64] = {};
    if (rest >= 56)
    {
        memcpy(last, p + full, rest);
        last[rest] = 0x80;
        md5_block(state, last);
        memset(last, 0, sizeof(last));
        memcpy(last, &bits, 4);
        memcpy(last + 60, &bits_tail, 4);
    }
    else
    {
        memcpy(last, &bits, 4);
        memcpy(last + 4, p + full, rest);
        last[4 + rest] = 0x80;
        memcpy(last + 60, &bits_tail, 4);
    }
    md5_block(state, last);
    memcpy(out, state, 16);
}

// ---------------------------------------------------------------- editing

namespace
{
uint32_t read32(const std::vector<uint8_t> &code, size_t at)
{
    uint32_t v;
    memcpy(&v, code.data() + at, 4);
    return v;
}

void write32(std::vector<uint8_t> &code, size_t at, uint32_t v)
{
    memcpy(code.data() + at, &v, 4);
}

// The byte offset of the SHEX (or SHDR) chunk's header, or 0.
size_t code_chunk(const std::vector<uint8_t> &code, uint32_t &index)
{
    if (code.size() < 32 || memcmp(code.data(), "DXBC", 4) != 0)
        return 0;
    const uint32_t count = read32(code, 28);
    if (32 + size_t(count) * 4 > code.size())
        return 0;
    for (index = 0; index < count; ++index)
    {
        const size_t at = read32(code, 32 + size_t(index) * 4);
        if (at + 16 <= code.size() &&
            (memcmp(code.data() + at, "SHEX", 4) == 0 || memcmp(code.data() + at, "SHDR", 4) == 0))
            return at;
    }
    return 0;
}
} // namespace

bool dxbc_insert(std::vector<uint8_t> &code, size_t at, const uint32_t *tokens, size_t count)
{
    uint32_t index = 0;
    const size_t chunk = code_chunk(code, index);
    if (chunk == 0)
        return false;
    const size_t stream = chunk + 8; // version token, then the length token
    const uint32_t length = read32(code, stream + 4);
    if (at < stream + 8 || at > stream + size_t(length) * 4 || (at - stream) % 4 != 0)
        return false;
    const size_t bytes = count * 4;
    code.insert(code.begin() + at, reinterpret_cast<const uint8_t *>(tokens),
                reinterpret_cast<const uint8_t *>(tokens) + bytes);
    write32(code, stream + 4, length + uint32_t(count));
    write32(code, chunk + 4, read32(code, chunk + 4) + uint32_t(bytes));
    write32(code, 24, uint32_t(code.size()));
    const uint32_t chunks = read32(code, 28);
    for (uint32_t i = 0; i < chunks; ++i)
    {
        const size_t slot = 32 + size_t(i) * 4;
        if (read32(code, slot) > chunk)
            write32(code, slot, read32(code, slot) + uint32_t(bytes));
    }
    return true;
}

bool dxbc_set_temps(std::vector<uint8_t> &code, uint32_t temps)
{
    uint32_t index = 0;
    const size_t chunk = code_chunk(code, index);
    if (chunk == 0)
        return false;
    const size_t stream = chunk + 8;
    const size_t end = stream + size_t(read32(code, stream + 4)) * 4;
    for (size_t p = stream + 8; p + 4 <= end;)
    {
        const uint32_t token = read32(code, p);
        const uint32_t op = token & 0x7FF;
        const uint32_t n = op == 53 ? read32(code, p + 4) : (token >> 24) & 0x7F; // 53: custom data
        if (n == 0)
            return false;
        if (op == 104) // dcl_temps
        {
            if (read32(code, p + 4) < temps)
                write32(code, p + 4, temps);
            return true;
        }
        p += size_t(n) * 4;
    }
    return false;
}
