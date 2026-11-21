#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "lod_hook.h"

#include <cstdio>
#include <cstring>

namespace
{
// The five stores of the category scales, then the next instruction (mov rax, [rdi+0x7240]).
const uint8_t PATTERN[] = {0xF3, 0x0F, 0x11, 0x87, 0x18, 0x18, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x8F, 0x1C, 0x18,
                           0x00, 0x00, 0xF3, 0x0F, 0x11, 0x97, 0x20, 0x18, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x9F,
                           0x24, 0x18, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0xA7, 0x28, 0x18, 0x00, 0x00, 0x48, 0x8B,
                           0x87, 0x40, 0x72, 0x00, 0x00};
const size_t STORES = 40; // the bytes the cave runs instead

// Cave layout. Code first, then the data it addresses rip-relative.
const size_t AT_DATA = 160;
const size_t AT_OBJECT = AT_DATA;    // 8 bytes: rdi
const size_t AT_MULT = AT_DATA + 8; // 5 floats

uint8_t *g_site = nullptr;
uint8_t *g_cave = nullptr;
uint64_t g_original = 0; // the site's first 8 bytes
bool g_active = false;
char g_status[160] = "LOD hook: not installed.";

uint8_t *find_site()
{
    auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage;
    uint8_t *found = nullptr;
    int count = 0;
    for (size_t at = 0; at < size;)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(base + at, &mbi, sizeof(mbi)) == 0)
            break;
        const size_t end = (reinterpret_cast<uint8_t *>(mbi.BaseAddress) - base) + mbi.RegionSize;
        const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & exec) != 0 && (mbi.Protect & PAGE_GUARD) == 0)
        {
            auto *p = reinterpret_cast<uint8_t *>(mbi.BaseAddress);
            for (size_t i = 0; i + sizeof(PATTERN) <= mbi.RegionSize; ++i)
                if (p[i] == PATTERN[0] && memcmp(p + i, PATTERN, sizeof(PATTERN)) == 0)
                {
                    found = p + i;
                    ++count;
                }
        }
        at = end;
    }
    return count == 1 ? found : nullptr;
}

// Executable memory within 2 GB below the site. A rel32 jump then reaches both ways.
uint8_t *alloc_near(uint8_t *site)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t step = si.dwAllocationGranularity;
    uintptr_t at = (reinterpret_cast<uintptr_t>(site) & ~(step - 1)) - step;
    for (int i = 0; i < 16384; ++i, at -= step)
    {
        void *p = VirtualAlloc(reinterpret_cast<void *>(at), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p != nullptr)
            return static_cast<uint8_t *>(p);
    }
    return nullptr;
}

void put32(uint8_t *at, int32_t v)
{
    memcpy(at, &v, 4);
}

// rel32 from the end of an instruction of `length` bytes at `ins` to `target`.
int32_t rel(uint8_t *ins, size_t length, uint8_t *target)
{
    return int32_t(target - (ins + length));
}

void build_cave()
{
    uint8_t *c = g_cave;
    size_t n = 0;
    // mov [rip+object], rdi
    c[n++] = 0x48; c[n++] = 0x89; c[n++] = 0x3D;
    put32(c + n, rel(c + n - 3, 7, g_cave + AT_OBJECT));
    n += 4;
    // mulss xmmK, [rip+mult K] for K = 0..4
    const uint8_t modrm[5] = {0x05, 0x0D, 0x15, 0x1D, 0x25};
    for (int k = 0; k < 5; ++k)
    {
        c[n++] = 0xF3; c[n++] = 0x0F; c[n++] = 0x59; c[n++] = modrm[k];
        put32(c + n, rel(c + n - 4, 8, g_cave + AT_MULT + 4 * k));
        n += 4;
    }
    // The same scaled values go back into +0x74EC..+0x74FC. Streaming reads that copy. Without it the two
    // disagree on the model and objects vanish. movss [rdi+disp32], xmmK.
    const uint8_t store[5] = {0x87, 0x8F, 0x97, 0x9F, 0xA7};
    for (int k = 0; k < 5; ++k)
    {
        c[n++] = 0xF3; c[n++] = 0x0F; c[n++] = 0x11; c[n++] = store[k];
        put32(c + n, int32_t(0x74EC + 4 * k));
        n += 4;
    }
    // The game's five stores, then back to the instruction after them.
    memcpy(c + n, g_site, STORES);
    n += STORES;
    c[n++] = 0xE9;
    put32(c + n, rel(c + n - 1, 5, g_site + STORES));
    n += 4;
    float one = 1.0f;
    for (int k = 0; k < 5; ++k)
        memcpy(g_cave + AT_MULT + 4 * k, &one, 4);
    memset(g_cave + AT_OBJECT, 0, 8);
    FlushInstructionCache(GetCurrentProcess(), g_cave, 4096);
}

bool write_site(uint64_t bytes)
{
    DWORD old = 0;
    if (!VirtualProtect(g_site, 8, PAGE_EXECUTE_READWRITE, &old))
        return false;
    // The site is 8-byte aligned. The game never sees half a jump.
    InterlockedExchange64(reinterpret_cast<volatile LONG64 *>(g_site), LONG64(bytes));
    VirtualProtect(g_site, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_site, 8);
    return true;
}
} // namespace

bool lod_hook_install()
{
    if (g_active)
        return true;
    if (g_site == nullptr)
    {
        g_site = find_site();
        if (g_site == nullptr)
        {
            snprintf(g_status, sizeof(g_status), "LOD hook: the category stores were not found exactly once.");
            return false;
        }
        if ((reinterpret_cast<uintptr_t>(g_site) & 7) != 0)
        {
            snprintf(g_status, sizeof(g_status), "LOD hook: the site is not 8-byte aligned. Not installed.");
            g_site = nullptr;
            return false;
        }
        memcpy(&g_original, g_site, 8);
    }
    if (g_cave == nullptr)
    {
        g_cave = alloc_near(g_site);
        if (g_cave == nullptr)
        {
            snprintf(g_status, sizeof(g_status), "LOD hook: no memory near the game's code.");
            return false;
        }
        build_cave();
    }
    uint8_t jump[8];
    memcpy(jump, &g_original, 8);
    jump[0] = 0xE9;
    put32(jump + 1, rel(g_site, 5, g_cave));
    uint64_t bytes;
    memcpy(&bytes, jump, 8);
    if (!write_site(bytes))
    {
        snprintf(g_status, sizeof(g_status), "LOD hook: the code could not be made writable.");
        return false;
    }
    g_active = true;
    snprintf(g_status, sizeof(g_status), "LOD hook: active at ACValhalla.exe+0x%llX.",
             static_cast<unsigned long long>(g_site - reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr))));
    return true;
}

void lod_hook_remove()
{
    if (!g_active)
        return;
    write_site(g_original); // the cave stays: a thread may still be in it
    g_active = false;
    snprintf(g_status, sizeof(g_status), "LOD hook: removed.");
}

bool lod_hook_active()
{
    return g_active;
}

void lod_hook_set(int category, float multiplier)
{
    if (g_cave != nullptr && category >= 0 && category < 5)
        InterlockedExchange(reinterpret_cast<volatile LONG *>(g_cave + AT_MULT + 4 * category),
                            *reinterpret_cast<LONG *>(&multiplier));
}

float lod_hook_get(int category)
{
    float v = 1.0f;
    if (g_cave != nullptr && category >= 0 && category < 5)
        memcpy(&v, g_cave + AT_MULT + 4 * category, 4);
    return v;
}

uintptr_t lod_hook_object()
{
    uintptr_t v = 0;
    if (g_cave != nullptr)
        memcpy(&v, g_cave + AT_OBJECT, 8);
    return v;
}

bool lod_hook_read(uint32_t offset, float &value)
{
    const uintptr_t object = lod_hook_object();
    SIZE_T got = 0;
    return object != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(object + offset), &value, 4,
                                             &got) &&
           got == 4;
}

const char *lod_hook_status()
{
    return g_status;
}
