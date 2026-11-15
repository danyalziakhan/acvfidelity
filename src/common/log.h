#pragma once

#include <string>

namespace reshade::log
{
    enum class level;
}

// Logs to acvfidelity.log next to the game's exe. Warnings and errors also go
// to ReShade's log with "ACV Fidelity: " in front.
void dd_log(reshade::log::level level, const char *message);
// The folder of the game's exe.
std::wstring game_folder();
// "1.2.3" from a file's version resource, or empty.
std::string file_version(const std::wstring &path);
// Writes the first lines of the log: versions, the game's detail settings and
// the mods in the game folder. Also starts logging access violations.
void log_start();

// Called after an access violation is logged.
extern void (*g_crash_hook)();
