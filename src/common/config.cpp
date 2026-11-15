#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "config.h"
#include "default_ini.h" // acvfidelity.ini from the repository, built in
#include "log.h"

#include <cmath>
#include <cstdio>
#include <string>

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

void config_set(const char *section, const char *key, int value)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    WritePrivateProfileStringW(wide(section).c_str(), wide(key).c_str(), wide(text).c_str(), g_path.c_str());
}

