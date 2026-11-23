#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "config.h"
#include "default_ini.h" // acvfidelity.ini from the repository, built in
#include "log.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
std::wstring g_path;

std::wstring wide(const char *text)
{
    wchar_t out[256];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out, 256);
    return out;
}
} // namespace

void config_open()
{
    g_path = game_folder() + L"\\acvfidelity.ini";
    // Written on the first start.
    if (GetFileAttributesW(g_path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (FILE *file = _wfopen(g_path.c_str(), L"wb"))
        {
            fwrite(DEFAULT_INI, 1, sizeof(DEFAULT_INI) - 1, file);
            fclose(file);
        }
    }
}

float config_float(const char *section, const char *key, float fallback)
{
    wchar_t value[64] = L"";
    GetPrivateProfileStringW(wide(section).c_str(), wide(key).c_str(), L"", value, 64, g_path.c_str());
    if (value[0] == 0)
        return fallback;
    wchar_t *end = nullptr;
    const double v = wcstod(value, &end);
    return end != value && std::isfinite(v) ? float(v) : fallback;
}

void config_set(const char *section, const char *key, float value)
{
    char text[32];
    snprintf(text, sizeof(text), "%g", value);
    WritePrivateProfileStringW(wide(section).c_str(), wide(key).c_str(), wide(text).c_str(), g_path.c_str());
}

static std::string narrow_text(const wchar_t *text, size_t length)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, int(length), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(size > 0 ? size : 0), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, text, int(length), out.data(), size, nullptr, nullptr);
    return out;
}

bool config_section(const char *section, std::vector<std::pair<std::string, std::string>> &lines)
{
    lines.clear();
    // The section names, to tell a missing section from an empty one.
    std::vector<wchar_t> names(65536);
    const DWORD got = GetPrivateProfileSectionNamesW(names.data(), DWORD(names.size()), g_path.c_str());
    bool found = false;
    for (const wchar_t *p = names.data(); p < names.data() + got && *p != 0; p += wcslen(p) + 1)
        found |= _wcsicmp(p, wide(section).c_str()) == 0;
    if (!found)
        return false;
    std::vector<wchar_t> text(65536);
    const DWORD size = GetPrivateProfileSectionW(wide(section).c_str(), text.data(), DWORD(text.size()), g_path.c_str());
    for (const wchar_t *p = text.data(); p < text.data() + size && *p != 0; p += wcslen(p) + 1)
    {
        if (*p == L';' || *p == L'#')
            continue;
        const wchar_t *eq = wcschr(p, L'=');
        std::string key = narrow_text(p, eq != nullptr ? size_t(eq - p) : wcslen(p));
        std::string value = eq != nullptr ? narrow_text(eq + 1, wcslen(eq + 1)) : std::string();
        auto trim = [](std::string &s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
        };
        trim(key);
        trim(value);
        lines.emplace_back(key, value);
    }
    return true;
}

void config_remove_section(const char *section)
{
    WritePrivateProfileStringW(wide(section).c_str(), nullptr, nullptr, g_path.c_str());
}

void config_remove_key(const char *section, const char *key)
{
    WritePrivateProfileStringW(wide(section).c_str(), wide(key).c_str(), nullptr, g_path.c_str());
}

void config_set(const char *section, const char *key, int value)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    WritePrivateProfileStringW(wide(section).c_str(), wide(key).c_str(), wide(text).c_str(), g_path.c_str());
}

