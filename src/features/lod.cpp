// The instruction is the same in every build since 1.7.0, including the Steam
// build after the New Game+ update:
//
//   C7 87 00 75 00 00 | 00 00 80 3F    mov dword ptr [rdi+0x7500], 1.0f
//   F3 0F 11 87 D4 74 00 00            movss [rdi+0x74D4], xmm0
//   EB ..                              jmp
//
// The same function writes 1.0 to that field a second time, followed by a
// call instead of the jmp. That branch overwrites the field with the call's
// result right after. It is left alone.
//
// The pattern is searched for in the game's executable memory instead of at a
// fixed address. That way one add-on covers the Steam, Ubisoft Connect and
// Ubisoft+ builds. In a build where the code differs it finds nothing and
// changes nothing.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "lod.h"
#include "log.h"

#include <reshade.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
const unsigned char CODE[] = {0xC7, 0x87, 0x00, 0x75, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F,
                                 0xF3, 0x0F, 0x11, 0x87, 0xD4, 0x74, 0x00, 0x00, 0xEB};
const size_t VALUE_OFFSET = 6; // the 1.0f immediate inside the pattern

unsigned char *g_value = nullptr;
std::string g_where = "not looked for yet";

bool executable(DWORD protect)
{
    return (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 &&
           (protect & PAGE_GUARD) == 0;
}
} // namespace

bool lod_find()
{
    if (g_value != nullptr)
        return true;
    auto *base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    unsigned char *const end = base + nt->OptionalHeader.SizeOfImage;

    // Only committed, executable pages of the game's image are read.
    unsigned char *found = nullptr;
    int matches = 0;
    MEMORY_BASIC_INFORMATION region;
    for (unsigned char *at = base; at < end && VirtualQuery(at, &region, sizeof(region)) == sizeof(region);
         at = static_cast<unsigned char *>(region.BaseAddress) + region.RegionSize)
    {
        if (region.State != MEM_COMMIT || !executable(region.Protect))
            continue;
        unsigned char *from = static_cast<unsigned char *>(region.BaseAddress);
        unsigned char *to = std::min(from + region.RegionSize, end);
        for (unsigned char *p = from; p + sizeof(CODE) <= to; ++p)
        {
            p = static_cast<unsigned char *>(memchr(p, CODE[0], size_t(to - p)));
            if (p == nullptr || p + sizeof(CODE) > to)
                break;
            if (memcmp(p, CODE, sizeof(CODE)) == 0)
            {
                found = p;
                ++matches;
            }
        }
    }

    char text[160];
    if (matches != 1)
    {
        snprintf(text, sizeof(text),
                 matches == 0 ? "the LOD factor's code was not found in this game build. Nothing is changed"
                              : "the LOD factor's code was found %d times. Nothing is changed",
                 matches);
        g_where = text;
        dd_log(reshade::log::level::warning, text);
        return false;
    }
    g_value = found + VALUE_OFFSET;
    snprintf(text, sizeof(text), "found the LOD factor at ACValhalla.exe+0x%llx",
             static_cast<unsigned long long>(found - base));
    g_where = text;
    dd_log(reshade::log::level::info, text);
    return true;
}

bool lod_found()
{
    return g_value != nullptr;
}

const std::string &lod_where()
{
    return g_where;
}

void lod_write(float factor)
{
    if (g_value == nullptr)
        return;
    DWORD old = 0;
    if (!VirtualProtect(g_value, sizeof(factor), PAGE_EXECUTE_READWRITE, &old))
    {
        dd_log(reshade::log::level::warning, "could not unprotect the LOD factor's code");
        return;
    }
    memcpy(g_value, &factor, sizeof(factor));
    VirtualProtect(g_value, sizeof(factor), old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_value, sizeof(factor));
}

float lod_current()
{
    float factor = 1.0f;
    if (g_value != nullptr)
        memcpy(&factor, g_value, sizeof(factor));
    return factor;
}
