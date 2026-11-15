// The add-on's own log is acvfidelity.log next to the game's exe. It starts
// fresh each session and is flushed line by line. A crash keeps everything
// written before it. Warnings and errors also go to ReShade's log and show in
// its Log tab.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include "log.h"

#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>

namespace
{
const char PREFIX[] = "ACV Fidelity: ";

std::mutex g_lock;
FILE *g_file = nullptr;
bool g_opened = false;

FILE *log_file()
{
    if (!g_opened)
    {
        g_opened = true;
        g_file = _wfopen((game_folder() + L"\\acvfidelity.log").c_str(), L"w");
    }
    return g_file;
}
} // namespace

std::string file_version(const std::wstring &path)
{
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size == 0)
        return {};
    std::string data(size, '\0');
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) || info == nullptr)
        return {};
    char text[32];
    snprintf(text, sizeof(text), "%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
             HIWORD(info->dwFileVersionLS));
    return text;
}

static std::string module_path(HMODULE module)
{
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(module, path, MAX_PATH);
    const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string text(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, text.data(), n, nullptr, nullptr);
    return text;
}

void (*g_crash_hook)() = nullptr;

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *info);
static void log_modules();

// The quoted value after a key in one of Steam's .acf files, or empty.
static std::string acf_value(const std::string &text, const char *key)
{
    const std::string quoted = std::string("\"") + key + "\"";
    size_t at = text.find(quoted);
    if (at == std::string::npos)
        return {};
    at = text.find('"', at + quoted.size());
    const size_t end = at == std::string::npos ? at : text.find('"', at + 1);
    return end == std::string::npos ? std::string() : text.substr(at + 1, end - at - 1);
}

// The game's exe carries no version. A Steam copy has its build number in the
// app manifest next to the library's common folder. Any other copy logs the
// exe's date instead, Ubisoft Connect's included. Only files are read.
static std::string game_build()
{
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring path(exe);
    const size_t folder_end = path.find_last_of(L'\\');
    const size_t common_end = folder_end == std::wstring::npos ? folder_end : path.find_last_of(L'\\', folder_end - 1);
    if (common_end != std::wstring::npos)
    {
        const std::wstring folder = path.substr(common_end + 1, folder_end - common_end - 1);
        const std::wstring steamapps = path.substr(0, common_end) + L"\\..";
        WIN32_FIND_DATAW found;
        const HANDLE search = FindFirstFileW((steamapps + L"\\appmanifest_*.acf").c_str(), &found);
        if (search != INVALID_HANDLE_VALUE)
        {
            std::string build;
            do
            {
                FILE *file = _wfopen((steamapps + L"\\" + found.cFileName).c_str(), L"rb");
                if (file == nullptr)
                    continue;
                std::string text(size_t(std::min<DWORD>(found.nFileSizeLow, 1 << 16)), '\0');
                text.resize(fread(text.data(), 1, text.size(), file));
                fclose(file);
                const std::string dir = acf_value(text, "installdir");
                const int n = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), int(dir.size()), nullptr, 0);
                std::wstring wide(size_t(n), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), int(dir.size()), wide.data(), n);
                if (!wide.empty() && _wcsicmp(wide.c_str(), folder.c_str()) == 0)
                    build = acf_value(text, "buildid");
            } while (build.empty() && FindNextFileW(search, &found));
            FindClose(search);
            if (!build.empty())
                return "Steam build " + build;
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    SYSTEMTIME date;
    if (!GetFileAttributesExW(exe, GetFileExInfoStandard, &data) || !FileTimeToSystemTime(&data.ftLastWriteTime, &date))
        return "unknown";
    char text[48];
    snprintf(text, sizeof(text), "exe dated %04u-%02u-%02u", date.wYear, date.wMonth, date.wDay);
    return text;
}

// What a bug report needs first: the versions and the game settings that
// change what the add-on does.
void log_start()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&log_start), &self);
    const auto version = [](HMODULE module) {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(module, path, MAX_PATH);
        const std::string text = file_version(path);
        return text.empty() ? std::string("unknown") : text;
    };
    std::string text = "ACV Fidelity " + version(self) + ", ReShade " +
                       version(reshade::internal::get_reshade_module_handle()) + ", game " + game_build() + " at " +
                       module_path(nullptr);
    dd_log(reshade::log::level::info, text.c_str());
    AddVectoredExceptionHandler(0, on_exception);
    log_modules();

    // The settings file the game will start with.
    wchar_t documents[MAX_PATH] = L"";
    if (SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents) != S_OK)
        return;
    const std::wstring ini = std::wstring(documents) + L"\\Assassin's Creed Valhalla\\ACValhalla.ini";
    text = "game settings:";
    for (const wchar_t *key : {L"FullscreenWidth", L"FullscreenHeight", L"PixelDensity", L"Clutter",
                               L"Environment", L"Terrain", L"Texture", L"Culling", L"Character", L"Shadow"})
    {
        wchar_t value[32] = L"";
        GetPrivateProfileStringW(L"Graphics", key, L"?", value, DWORD(std::size(value)), ini.c_str());
        char line[96];
        snprintf(line, sizeof(line), " %ls=%ls", key, value);
        text += line;
    }
    dd_log(reshade::log::level::info, text.c_str());
}

// "ACValhalla.exe+0x1234" for a code address. The bare address outside any module.
static std::string where(DWORD64 address)
{
    HMODULE module = nullptr;
    char text[160];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        module == nullptr)
    {
        snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        return text;
    }
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t *name = wcsrchr(path, L'\\');
    snprintf(text, sizeof(text), "%ls+0x%llx", name != nullptr ? name + 1 : path,
             static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    return text;
}

// The game's own crash handler ends the process without a word in our log.
// So access violations are logged where they happen, with the module, the
// offset and the call stack. A game can also raise and handle one on purpose.
// That is why the count is capped and the line says "if the game crashed".
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<int> written{0};
    if (written++ >= 3)
        return EXCEPTION_CONTINUE_SEARCH;

    const ULONG_PTR *params = info->ExceptionRecord->ExceptionInformation;
    const char *access = params[0] == 0 ? "reading" : params[0] == 1 ? "writing" : "executing";
    char text[160];
    snprintf(text, sizeof(text), "access violation %s 0x%llx at %s (if the game crashed, this is where)", access,
             static_cast<unsigned long long>(params[1]), where(DWORD64(info->ExceptionRecord->ExceptionAddress)).c_str());
    dd_log(reshade::log::level::error, text);

    CONTEXT context = *info->ContextRecord;
    std::string stack = "call stack:";
    for (int i = 0; i < 24 && context.Rip != 0; ++i)
    {
        stack += " " + where(context.Rip);
        DWORD64 image = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
        if (function == nullptr)
        {
            // A leaf function: the return address is on top of the stack.
            context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void *handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &frame, nullptr);
    }
    dd_log(reshade::log::level::error, stack.c_str());
    if (g_crash_hook != nullptr)
        g_crash_hook();
    return EXCEPTION_CONTINUE_SEARCH;
}

// Logs every module loaded from the game's folder and its version. Mods live
// there, and another mod that changes the LOD is the first thing to rule out.
static void log_modules()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    std::wstring folder = game_folder();
    for (wchar_t &ch : folder)
        ch = wchar_t(towlower(ch));
    std::string in_folder;
    MODULEENTRY32W entry = {sizeof(entry)};
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry))
    {
        std::wstring path = entry.szExePath;
        std::wstring lower = path;
        for (wchar_t &ch : lower)
            ch = wchar_t(towlower(ch));
        const std::wstring name = lower.substr(lower.find_last_of(L'\\') + 1);
        const bool is_mod = lower.compare(0, folder.size(), folder) == 0 && name != L"acvalhalla.exe";
        if (!is_mod)
            continue;
        const std::string version = file_version(path);
        char item[MAX_PATH + 64];
        snprintf(item, sizeof(item), "%s%ls%s%s", in_folder.empty() ? "" : ", ", entry.szModule,
                 version.empty() ? "" : " ", version.c_str());
        in_folder += item;
    }
    CloseHandle(snapshot);
    dd_log(reshade::log::level::info, ("loaded from the game folder: " + in_folder).c_str());
}

std::wstring game_folder()
{
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const std::wstring exe(path);
    return exe.substr(0, exe.find_last_of(L"\\/"));
}

void dd_log(reshade::log::level level, const char *message)
{
    const bool prefixed = strncmp(message, PREFIX, sizeof(PREFIX) - 1) == 0;
    if (level == reshade::log::level::error || level == reshade::log::level::warning)
        reshade::log::message(level, prefixed ? message : (std::string(PREFIX) + message).c_str());

    const char *text = prefixed ? message + sizeof(PREFIX) - 1 : message;
    const char *tag = level == reshade::log::level::error     ? "ERROR"
                      : level == reshade::log::level::warning ? "WARN "
                                                              : "INFO ";
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::lock_guard lock(g_lock);
    if (FILE *file = log_file())
    {
        fprintf(file, "%02u:%02u:%02u:%03u [%5lu] | %s | %s\n", now.wHour, now.wMinute, now.wSecond,
                now.wMilliseconds, GetCurrentThreadId(), tag, text);
        fflush(file);
    }
}
