// ACV Fidelity
//
// Takes Assassin's Creed Valhalla's graphics past its own highest settings.
// Ambient occlusion is in ao.cpp and reflections in ssr.cpp. Level of detail lowers the game's LOD factor
// (lod.cpp) to keep the detailed models on screen farther away. The per-category distances and the pause
// during loads are in lod_control.cpp.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "ao.h"
#include "config.h"
#include "diagnostics.h"
#include "light.h"
#include "log.h"
#include "grass.h"
#include "lod.h"
#include "lod_control.h"
#include "lod_fade.h"
#include "ssr_angle.h"
#include "lod_hook.h"
#include "scene_inject.h"
#include "scene_state.h"
#include "ssil.h"
#include "ssr.h"
#include "ssr2.h"

#include <algorithm>
#include <random>
#include <sstream>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "ACV Fidelity";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Raises Assassin's Creed Valhalla's image quality past its own settings.";

namespace
{

// The detail distance stops at 2.5x (factor 0.4). Nothing more was visible past it. Lower factors brought
// geometry glitches and crashes in testing. The categories stop at 3.0x on top of it, as tested in game.
const float MAX_DISTANCE = 2.5f;
// Objects and foliage at 3.5x made distant trees flicker and break on the ground (2026-11-24). 3x held up.
const float MAX_CATEGORY = 3.0f;
// Clutter only covers small plants near the player. Their spawn radius stays fixed. So it can go further.
const float MAX_CLUTTER = 4.0f;
// Grass: above 3x plants blinked and stretched at full raven height and the GPU hung, before the LOD guard. With
// the guard holding Fidelity I values in the air, 4x held up on the ground and at full raven height.
const float MAX_GRASS = 4.0f;
// The window's level of detail values: detail distance, the categories, then grass.
const int LOD_GRASS = 1 + LOD_CATEGORIES;
const int LOD_VALUES = 2 + LOD_CATEGORIES;
// Game, Fidelity I, Fidelity II, Fidelity III: detail distance, then objects, clutter, foliage and grass.
const float LOD_PRESETS[PRESETS][LOD_VALUES] = {
    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {1.5f, 1.67f, 2.0f, 1.67f, 2.0f},
    {2.0f, 2.33f, 3.0f, 2.33f, 3.0f},
    {2.5f, 3.0f, 4.0f, 3.0f, 4.0f},
};
const char *const LOD_KEYS[LOD_VALUES] = {"DetailDistance", "ObjectsDistance", "ClutterDetail", "FoliageDistance",
                                          "GrassDistance"};

// The window's level of detail values, in the order of LOD_PRESETS.
float g_lod[LOD_VALUES] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
// Model fade in seconds, per preset. A longer fade made pop-in more visible while riding. 0.25 s looked best.
const float FADE_PRESETS[PRESETS] = {0.5f, 0.4f, 0.3f, 0.2f};
const float MIN_FADE = 0.15f, MAX_FADE = 0.5f;
float g_lod_fade = 0.5f;
// Streaming distance of the main and long-range grids. Past 2x far detail dropped on 8 GB cards (video memory).
const float STREAM_PRESETS[PRESETS] = {1.0f, 2.0f, 2.0f, 2.0f};
const float MAX_STREAM = 2.0f;
float g_streaming = 1.0f;
float g_ao[AO_PARAMS];
float g_ssr[SSR_PARAMS];
float g_mat[MAT_PARAMS];
float g_ssil[SSIL_PARAMS];

// The master switch. Off gives the game its own values in every section. The settings stay as they are.
bool g_enabled = true;
// F11 by default. F9 is the game's quick load and F10 is ACV DLSS's key.
int g_toggle_key = VK_F11;
bool g_listening = false;
int g_listen_frame = -1;
const auto INDICATOR_TIME = std::chrono::seconds(3);
std::chrono::steady_clock::time_point g_indicator_until;
// ACV DLSS shows "DLSS: On/Off" in the same corner for 3 s after its toggle key. Our line goes below it then and
// takes the corner otherwise. Its key comes from acvdlss.ini, read again whenever the overlay closes.
int g_dlss_key = 0;
std::chrono::steady_clock::time_point g_dlss_indicator_until;
bool g_overlay_open = false;

// Loads a section's values from acvfidelity.ini. A missing value takes the default preset's.
void load_values(const char *section, const param_def *defs, const float *presets, int count, int preset,
                 float *values)
{
    for (int i = 0; i < count; ++i)
    {
        float v = presets[preset * count + i];
        // Only the settings shown in the window are read. The rest keep the preset's values.
        if (!defs[i].advanced)
            v = config_float(section, defs[i].key, v);
        values[i] = std::isfinite(v) ? std::clamp(v, defs[i].min, defs[i].max) : presets[preset * count + i];
    }
}

void save_value(const char *section, const param_def &def, float value);

// A section of acvfidelity.ini that is not exactly this version's (an old section name, a key this version doesn't
// know, a key it needs missing, a value that doesn't parse or is out of range) is set to the default preset,
// Fidelity II, and written back clean. No partial migration of old values. One log line per section reset.
struct ini_key
{
    const char *key;
    float min, max;
    bool required; // written by every save (the window's values)
};

// Why a section is not this version's, or empty. *missing when the section isn't there at all.
std::string section_problem(const char *section, const std::vector<ini_key> &keys, bool *missing)
{
    std::vector<std::pair<std::string, std::string>> lines;
    *missing = !config_section(section, lines);
    if (*missing)
        return "";
    for (const auto &[key, value] : lines)
    {
        const ini_key *k = nullptr;
        for (const ini_key &x : keys)
            if (_stricmp(x.key, key.c_str()) == 0)
                k = &x;
        if (k == nullptr)
            return "unknown key " + key;
        char *end = nullptr;
        const double v = strtod(value.c_str(), &end);
        if (end == value.c_str() || *end != 0 || !std::isfinite(v) || v < k->min - 1e-4 || v > k->max + 1e-4)
            return key + " = " + value + " is not valid";
    }
    for (const ini_key &x : keys)
    {
        bool there = false;
        for (const auto &line : lines)
            there |= _stricmp(line.first.c_str(), x.key) == 0;
        if (x.required && !there)
            return std::string("missing key ") + x.key;
    }
    return "";
}

// Checks a section; if it is missing or not this version's, writes the default preset's values. Old section
// names are removed first. True when the section was written.
bool check_section(const char *section, const std::vector<ini_key> &keys, std::initializer_list<const char *> old,
                   const std::function<void()> &write_defaults)
{
    std::string reason;
    for (const char *name : old)
    {
        std::vector<std::pair<std::string, std::string>> lines;
        if (config_section(name, lines))
        {
            config_remove_section(name);
            reason = std::string("old section [") + name + "]";
        }
    }
    bool missing = false;
    const std::string problem = section_problem(section, keys, &missing);
    if (reason.empty())
        reason = problem;
    if (!missing && problem.empty() && reason.empty())
        return false;
    config_remove_section(section);
    write_defaults();
    if (!reason.empty())
    {
        char text[256];
        snprintf(text, sizeof(text), "acvfidelity.ini: [%s] set to Fidelity II (%s)", section, reason.c_str());
        dd_log(reshade::log::level::warning, text);
    }
    return true;
}

std::vector<ini_key> keys_of(const param_def *defs, int count)
{
    std::vector<ini_key> keys;
    // Only the window's settings. A key the window doesn't show (one from an older version) is not this version's.
    for (int i = 0; i < count; ++i)
        if (!defs[i].advanced)
            keys.push_back({defs[i].key, defs[i].min, defs[i].max, true});
    return keys;
}

// The default preset's values of a tuning section, as a save writes them.
void write_preset(const char *section, const param_def *defs, const float *presets, int count, int preset)
{
    for (int i = 0; i < count; ++i)
        if (!defs[i].advanced)
            save_value(section, defs[i], presets[preset * count + i]);
}

void save_lod();

float grass_for(float clutter);

void load_settings()
{
    config_open();
    g_enabled = config_float("General", "Enabled", 1.0f) != 0.0f;
    {
        const float key = config_float("General", "ToggleKey", float(VK_F11));
        g_toggle_key = key >= 0.0f && key < 256.0f ? int(key) : VK_F11;
    }
    {
        std::vector<ini_key> keys;
        for (int i = 0; i < LOD_VALUES; ++i)
        {
            const float max = i == 0                 ? MAX_DISTANCE
                              : i == 1 + LOD_CLUTTER ? MAX_CLUTTER
                              : i == LOD_GRASS       ? MAX_GRASS
                                                     : MAX_CATEGORY;
            // Grass follows clutter in the release window; a save writes it all the same.
            keys.push_back({LOD_KEYS[i], 1.0f, max, i != LOD_GRASS});
        }
        keys.push_back({"ModelFade", MIN_FADE, MAX_FADE, true});
        keys.push_back({"StreamingDistance", 1.0f, MAX_STREAM, true});
        check_section("LevelOfDetail", keys, {}, [] {
            std::copy(LOD_PRESETS[2], LOD_PRESETS[2] + LOD_VALUES, g_lod);
            g_lod_fade = FADE_PRESETS[2];
            g_streaming = STREAM_PRESETS[2];
            save_lod();
        });
    }
    check_section("AmbientOcclusion", keys_of(AO_DEFS, AO_PARAMS), {}, [] {
        write_preset("AmbientOcclusion", AO_DEFS, &AO_PRESET_VALUES[0][0], AO_PARAMS, AO_DEFAULT_PRESET);
    });
    check_section("ScreenSpaceReflectionsWater", keys_of(SSR_DEFS, SSR_PARAMS), {"ScreenSpaceReflections"}, [] {
        write_preset("ScreenSpaceReflectionsWater", SSR_DEFS, &SSR_PRESET_VALUES[0][0], SSR_PARAMS, SSR_DEFAULT_PRESET);
    });
    check_section("ScreenSpaceReflectionsSurfaces", keys_of(MAT_DEFS, MAT_PARAMS), {}, [] {
        write_preset("ScreenSpaceReflectionsSurfaces", MAT_DEFS, &MAT_PRESET_VALUES[0][0], MAT_PARAMS,
                     MAT_DEFAULT_PRESET);
    });
    check_section("IndirectLighting", keys_of(SSIL_DEFS, SSIL_PARAMS), {}, [] {
        write_preset("IndirectLighting", SSIL_DEFS, &SSIL_PRESET_VALUES[0][0], SSIL_PARAMS, SSIL_DEFAULT_PRESET);
    });
    for (int i = 0; i < LOD_VALUES; ++i)
    {
        const float v = config_float("LevelOfDetail", LOD_KEYS[i], LOD_PRESETS[2][i]);
        const float max = i == 0                 ? MAX_DISTANCE
                          : i == 1 + LOD_CLUTTER ? MAX_CLUTTER
                          : i == LOD_GRASS       ? MAX_GRASS
                                                 : MAX_CATEGORY;
        g_lod[i] = std::isfinite(v) ? std::clamp(v, 1.0f, max) : LOD_PRESETS[2][i];
    }
    // One slider sets clutter and grass together.
    g_lod[LOD_GRASS] = grass_for(g_lod[1 + LOD_CLUTTER]);
    {
        const float v = config_float("LevelOfDetail", "ModelFade", FADE_PRESETS[2]);
        g_lod_fade = std::isfinite(v) ? std::clamp(v, MIN_FADE, MAX_FADE) : FADE_PRESETS[2];
    }
    {
        const float v = config_float("LevelOfDetail", "StreamingDistance", STREAM_PRESETS[2]);
        g_streaming = std::isfinite(v) ? std::clamp(v, 1.0f, MAX_STREAM) : STREAM_PRESETS[2];
    }
    load_values("AmbientOcclusion", AO_DEFS, &AO_PRESET_VALUES[0][0], AO_PARAMS, AO_DEFAULT_PRESET, g_ao);
    load_values("ScreenSpaceReflectionsWater", SSR_DEFS, &SSR_PRESET_VALUES[0][0], SSR_PARAMS, SSR_DEFAULT_PRESET, g_ssr);
    load_values("ScreenSpaceReflectionsSurfaces", MAT_DEFS, &MAT_PRESET_VALUES[0][0], MAT_PARAMS, MAT_DEFAULT_PRESET,
                g_mat);
    load_values("IndirectLighting", SSIL_DEFS, &SSIL_PRESET_VALUES[0][0], SSIL_PARAMS, SSIL_DEFAULT_PRESET, g_ssil);
}

// The surface reflections and the indirect lighting. The indirect light's thickness is the AO section's bitmask
// thickness.
void scene_set(bool enabled)
{
    ssr2_set(enabled ? g_mat : MAT_PRESET_VALUES[0]);
    // Indirect lighting needs the bitmask: with Horizon-based AO it is off, its values kept.
    ssil_set(enabled && g_ao[AO_METHOD] >= 0.5f ? g_ssil : SSIL_PRESET_VALUES[0], g_ao[AO_THICKNESS]);
}


// Hands the values to the LOD control. It eases the game toward them.
void push_lod(const float *values)
{
    lod_control_set(values[0], values + 1, values[LOD_GRASS]);
    // The game's own 0.5 s: leave its value alone.
    lod_fade_set(g_lod_fade >= MAX_FADE - 0.001f ? 0.0f : g_lod_fade);
    grass_set_streaming(g_streaming);
}

void save_lod()
{
    for (int i = 0; i < LOD_VALUES; ++i)
        config_set("LevelOfDetail", LOD_KEYS[i], std::round(g_lod[i] * 100.0f) / 100.0f);
    config_set("LevelOfDetail", "ModelFade", std::round(g_lod_fade * 100.0f) / 100.0f);
    config_set("LevelOfDetail", "StreamingDistance", std::round(g_streaming * 100.0f) / 100.0f);
}




bool on_overlay(effect_runtime *, bool open, input_source)
{
    g_overlay_open = open;
    return false; // never block the overlay
}

// Applies the settings, or the game's own values while switched off. Level of detail comes back through its
// usual gradual climb, under the guard.
void apply_all()
{
    if (g_enabled)
    {
        push_lod(g_lod);
        ao_set(g_ao);
        ssr_set(g_ssr);
        scene_set(true);
        return;
    }
    const float categories[LOD_CATEGORIES] = {1.0f, 1.0f, 1.0f};
    lod_control_set(1.0f, categories, 1.0f);
    lod_fade_set(0.0f);
    grass_set_streaming(1.0f);
    ao_set(AO_PRESET_VALUES[0]);
    ssr_set(SSR_PRESET_VALUES[0]);
    scene_set(false);
}

void set_enabled(bool enabled, const char *how)
{
    g_enabled = enabled;
    config_set("General", "Enabled", enabled ? 1 : 0);
    apply_all();
    char text[96];
    snprintf(text, sizeof(text), "switched %s with %s", enabled ? "on" : "off", how);
    dd_log(reshade::log::level::info, text);
    g_indicator_until = std::chrono::steady_clock::now() + INDICATOR_TIME;
}

// A key ReShade already uses would trigger both.
bool is_reshade_key(int vk)
{
    for (const char *name : {"KeyOverlay", "KeyEffects", "KeyScreenshot", "KeyReload", "KeyNextPreset",
                             "KeyPreviousPreset", "KeyFPS", "KeyFrametime"})
    {
        char value[64] = "";
        size_t size = sizeof(value);
        unsigned key = 0;
        if (reshade::get_config_value(nullptr, "INPUT", name, value, &size) && sscanf(value, "%u", &key) == 1 &&
            int(key) == vk)
            return true;
    }
    return false;
}

std::string utf8(const wchar_t *text)
{
    char out[64];
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, sizeof(out), nullptr, nullptr);
    return n > 0 ? std::string(out) : std::string();
}

// As printed on the key, in the player's layout.
std::string key_name(uint32_t vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return std::string(1, char(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return "Numpad " + std::to_string(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F24)
        return "F" + std::to_string(vk - VK_F1 + 1);
    switch (vk)
    {
    case VK_MBUTTON: return "Middle Mouse";
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    case VK_PAUSE: return "Pause";
    case VK_CAPITAL: return "Caps Lock";
    case VK_ESCAPE: return "Escape";
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter";
    case VK_TAB: return "Tab";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_END: return "End";
    case VK_HOME: return "Home";
    case VK_LEFT: return "Left";
    case VK_UP: return "Up";
    case VK_RIGHT: return "Right";
    case VK_DOWN: return "Down";
    case VK_SNAPSHOT: return "Print Screen";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_MULTIPLY: return "Numpad *";
    case VK_ADD: return "Numpad +";
    case VK_SUBTRACT: return "Numpad -";
    case VK_DECIMAL: return "Numpad .";
    case VK_DIVIDE: return "Numpad /";
    case VK_NUMLOCK: return "Num Lock";
    case VK_SCROLL: return "Scroll Lock";
    }
    if (const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC); scan != 0)
    {
        wchar_t text[48] = L"";
        if (GetKeyNameTextW(LONG(scan << 16), text, int(std::size(text))) > 0)
            return utf8(text);
    }
    return "Key " + std::to_string(vk);
}

void draw_toggle_key(effect_runtime *runtime)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Toggle key");
    ImGui::SameLine();
    const std::string name = g_toggle_key != 0 ? key_name(g_toggle_key) : std::string("None");
    const std::string label = (g_listening ? std::string("Press a key or Esc to cancel") : name) + "###toggle_key";
    const int frame = ImGui::GetFrameCount();
    // Window closed while listening. Don't wait unseen.
    if (g_listening && frame != g_listen_frame + 1)
        g_listening = false;
    bool started = false;
    if (ImGui::Button(label.c_str(), ImVec2(220.0f * ImGui::GetFontSize() / 13.0f, 0.0f)) && !g_listening)
        g_listening = started = true;
    g_listen_frame = frame;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Switches ACV Fidelity on and off while playing.\n"
                          "Backspace while picking leaves it without a key.");
    // Windows reports every key as up while the overlay has input. Skip the first frame so the Space or Enter
    // that pressed the button isn't taken.
    if (!g_listening || started || runtime == nullptr)
        return;
    for (int vk = VK_MBUTTON; vk < 256; ++vk)
    {
        // The generic modifier codes repeat their left and right forms.
        if (!runtime->is_key_pressed(vk) || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
            continue;
        if (vk != VK_ESCAPE && is_reshade_key(vk))
            continue;
        if (vk != VK_ESCAPE)
        {
            g_toggle_key = vk == VK_BACK ? 0 : vk;
            config_set("General", "ToggleKey", g_toggle_key);
            char text[96];
            snprintf(text, sizeof(text), "toggle key set to %s",
                     g_toggle_key != 0 ? key_name(g_toggle_key).c_str() : "none");
            dd_log(reshade::log::level::info, text);
        }
        g_listening = false;
        break;
    }
}

// "Fidelity: On" or "Off" for a few seconds after the toggle key, overlay open or not. Below ACV DLSS's line.
void on_overlay_draw(effect_runtime *)
{
    if (std::chrono::steady_clock::now() >= g_indicator_until)
        return;
    const bool below = std::chrono::steady_clock::now() < g_dlss_indicator_until;
    ImGui::SetNextWindowPos(ImVec2(16.0f, below ? 16.0f + 3.0f * ImGui::GetFontSize() : 16.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##acvfidelity_indicator", nullptr, flags))
    {
        ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * 1.3f);
        if (g_enabled)
            ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "Fidelity: On");
        else
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1.0f), "Fidelity: Off");
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}



// The DLSS toggle key when ACV DLSS is loaded, else 0.
int read_dlss_key()
{
    if (GetModuleHandleW(L"acvdlss.addon64") == nullptr)
        return 0;
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return 0;
    std::wstring ini(path, n);
    ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"acvdlss.ini";
    const UINT key = GetPrivateProfileIntW(L"General", L"ToggleKey", VK_F10, ini.c_str());
    return key < 256 ? int(key) : 0;
}

void on_present(effect_runtime *runtime)
{
    static bool overlay_was_open = true;
    if (overlay_was_open && !g_overlay_open)
        g_dlss_key = read_dlss_key();
    overlay_was_open = g_overlay_open;
    if (!g_overlay_open && g_dlss_key != 0 && runtime->is_key_pressed(g_dlss_key))
        g_dlss_indicator_until = std::chrono::steady_clock::now() + INDICATOR_TIME;
    // At most one switch a second: each one reloads detail and rebuilds the reflection targets, and rapid
    // switching made the GPU fault (2026-11-25).
    static std::chrono::steady_clock::time_point last_toggle{};
    if (!g_overlay_open && g_toggle_key != 0 && runtime->is_key_pressed(g_toggle_key) &&
        std::chrono::steady_clock::now() - last_toggle >= std::chrono::seconds(1))
    {
        last_toggle = std::chrono::steady_clock::now();
        set_enabled(!g_enabled, "the toggle key");
    }
    diagnostics_present(runtime->get_command_queue());
    lod_control_frame();
    lod_fade_frame();
    ssr_angle_frame(lod_hook_object());
    // The surface reflections and the indirect lighting go on once the game has drawn a while.
    static int frames = 0;
    if (++frames == 600)
    {
        ssr2_start();
        ssil_start();
    }
    scene::present();
}

const ImVec4 WARNING_COLOR(0.96f, 0.73f, 0.24f, 1.0f);

// Grass distance for a clutter detail: the same, capped at the grass maximum. The 2:3 ratio of earlier versions
// dates from before the LOD guard.
float grass_for(float clutter)
{
    return std::min(std::round(clutter * 100.0f) / 100.0f, MAX_GRASS);
}

// One level of detail slider: a distance multiplier shown as "1.50x".
bool lod_slider(int i, const char *label, float max, const char *tooltip)
{
    float v = g_lod[i];
    ImGui::SliderFloat(label, &v, 1.0f, max, "%.2fx", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    const bool changed = v != g_lod[i];
    if (changed)
        g_lod[i] = std::round(v * 20.0f) / 20.0f;
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        char text[64];
        snprintf(text, sizeof(text), "%s set to %.2fx", label, g_lod[i]);
        dd_log(reshade::log::level::info, text);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    return changed;
}

// The preset the level of detail values match, or PRESETS (Custom) for none.
int lod_preset()
{
    for (int p = 0; p < PRESETS; ++p)
    {
        bool same = true;
        for (int i = 0; i < LOD_VALUES; ++i)
            same &= std::fabs(g_lod[i] - LOD_PRESETS[p][i]) < 0.005f;
        same &= std::fabs(g_lod_fade - FADE_PRESETS[p]) < 0.005f;
        same &= std::fabs(g_streaming - STREAM_PRESETS[p]) < 0.005f;
        if (same)
            return p;
    }
    return PRESETS;
}

void lod_apply_preset(int p)
{
    std::copy(LOD_PRESETS[p], LOD_PRESETS[p] + LOD_VALUES, g_lod);
    g_lod_fade = FADE_PRESETS[p];
    g_streaming = STREAM_PRESETS[p];
    save_lod();
    push_lod(g_lod);
}

void lod_section()
{
    if (!ImGui::CollapsingHeader("Level of detail"))
        return;
    if (!lod_found())
        return;
    ImGui::PushID("lod");
    const int current = lod_preset();
    bool changed = false;
    if (ImGui::BeginCombo("Preset", PRESET_NAMES[current]))
    {
        for (int p = 0; p < PRESETS; ++p)
            if (ImGui::Selectable(PRESET_NAMES[p], p == current) && p != current)
            {
                lod_apply_preset(p);
                char text[64];
                snprintf(text, sizeof(text), "Level of detail set to %s", PRESET_NAMES[p]);
                dd_log(reshade::log::level::info, text);
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sets every value below. Changing one afterward shows Custom.");
    changed |= lod_slider(0, "Detail distance", MAX_DISTANCE,
                          "How far away objects keep their detailed models.\n1.0x is the game's own.");
    if (lod_control_categories())
    {
        // Grass only shows with clutter detail to match. So one slider sets both.
        if (lod_slider(1 + LOD_CLUTTER, "Clutter distance", MAX_CLUTTER,
                       "How far out grass and small plants appear and how detailed they stay.\n"
                       "1.0x is the game's own."))
        {
            g_lod[LOD_GRASS] = grass_for(g_lod[1 + LOD_CLUTTER]);
            changed = true;
        }
    }
    if (lod_control_categories())
    {
        changed |= lod_slider(1 + LOD_OBJECTS, "Objects distance", MAX_CATEGORY,
                              "Rocks, props, bushes and some trees on top of the detail distance.\n"
                              "1.0x is the game's own.");
        changed |= lod_slider(1 + LOD_FOLIAGE, "Foliage distance", MAX_CATEGORY,
                              "Leaves and branches of trees and large plants on top of the\n"
                              "detail distance. 1.0x is the game's own.");
    }
    {
        // It follows grass: at 1.0x clutter the game's own streaming stays.
        const bool inactive = lod_control_categories() && g_lod[LOD_GRASS] <= 1.0f;
        ImGui::BeginDisabled(inactive);
        float v = g_streaming;
        ImGui::SliderFloat("Streaming distance", &v, 1.0f, MAX_STREAM, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        if (v != g_streaming)
        {
            g_streaming = std::round(v * 20.0f) / 20.0f;
            changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            char text[64];
            snprintf(text, sizeof(text), "Streaming distance set to %.2fx", g_streaming);
            dd_log(reshade::log::level::info, text);
        }
        ImGui::EndDisabled();
        if (inactive && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("No effect while clutter distance is at 1.0x.");
        else if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How far out the game loads detailed models and far tree lines.\n"
                              "1.0x is the game's own. More uses more video memory.");
    }
    {
        float v = g_lod_fade;
        ImGui::SliderFloat("Model fade", &v, MIN_FADE, MAX_FADE, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
        if (v != g_lod_fade)
        {
            g_lod_fade = std::round(v * 100.0f) / 100.0f;
            changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            char text[64];
            snprintf(text, sizeof(text), "Model fade set to %.2f s", g_lod_fade);
            dd_log(reshade::log::level::info, text);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How long a model takes to blend into its next level of detail.\n"
                              "Shorter makes the change less noticeable. 0.50 s is the game's own.");
    }
    if (changed)
    {
        save_lod();
        push_lod(g_lod);
    }
    ImGui::PopID();
}

void save_value(const char *section, const param_def &def, float value)
{
    config_set(section, def.key, value);
}

// The preset the values match, or PRESETS (Custom) for none.
int preset_of(const float *values, const float *presets, int count)
{
    for (int p = 0; p < PRESETS; ++p)
        if (std::equal(values, values + count, presets + p * count))
            return p;
    return PRESETS;
}

// Draws a section: the preset combo and the sliders. The Game preset sets
// the game's own values. Values apply when a slider is let go. Each change makes
// new shader copies. True when something changed.
// Writes every shown value of a section to the ini.
void save_section(const char *section, const param_def *defs, const float *values, int count)
{
    for (int i = 0; i < count; ++i)
        if (!defs[i].advanced)
            save_value(section, defs[i], values[i]);
}

// Shown on the indirect lighting's controls while AO is Horizon-based.
const char *const SSIL_NEEDS_BITMASK = "Needs the Visibility bitmask AO method.";

bool tuning_section(const char *title, const char *section, const param_def *defs, const float *presets,
                    int count, float *values, const int *order)
{
    if (!ImGui::CollapsingHeader(title))
        return false;
    ImGui::PushID(title);
    bool changed = false;
    auto save_all = [&] { save_section(section, defs, values, count); };
    const int current = preset_of(values, presets, count);
    const bool section_off = defs == SSIL_DEFS && g_ao[AO_METHOD] < 0.5f;
    if (section_off)
        ImGui::TextDisabled("Works only with the Visibility bitmask AO method.");
    ImGui::BeginDisabled(section_off);
    if (ImGui::BeginCombo("Preset", PRESET_NAMES[current]))
    {
        for (int p = 0; p < PRESETS; ++p)
            if (ImGui::Selectable(PRESET_NAMES[p], p == current) && p != current)
            {
                std::copy(presets + p * count, presets + (p + 1) * count, values);
                save_all();
                changed = true;
                char text[64];
                snprintf(text, sizeof(text), "%s set to %s", title, PRESET_NAMES[p]);
                dd_log(reshade::log::level::info, text);
            }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (section_off && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", SSIL_NEEDS_BITMASK);
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sets every value below. Changing one afterward shows Custom.");

    auto slider = [&](int i) {
        const param_def &def = defs[i];
        // A setting the chosen method doesn't use stays in place, greyed out.
        const char *inactive = defs == AO_DEFS    ? ao_inactive_note(i, values)
                               : defs == SSR_DEFS ? ssr_inactive_note(i, values)
                               : section_off      ? SSIL_NEEDS_BITMASK
                                                  : nullptr;
        ImGui::BeginDisabled(inactive != nullptr);
        ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
        if (def.logarithmic)
            flags |= ImGuiSliderFlags_Logarithmic;
        const char *name = ssr_value_name(def, values[i]);
        if (name == nullptr && &def == &MAT_DEFS[MAT_SCALE])
            name = values[i] < 0.625f ? "Half" : values[i] < 0.875f ? "Three quarters" : "Full";
        if (name == nullptr)
            name = ao_value_name(def, values[i]);
        // Indirect lighting's strength goes down to 0.1 on the slider. 0 (off) is the Game preset.
        const float low = defs == SSIL_DEFS && i == SSIL_STRENGTH && values[i] > 0.0f ? 0.1f : def.min;
        ImGui::SliderFloat(def.label, &values[i], low, def.max, name != nullptr ? name : def.format, flags);
        if (def.integer)
            values[i] = std::round(values[i]);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            // Rounded to the decimals shown. The ini then holds what the window showed and presets match again.
            if (const char *dot = strchr(def.format, '.'); dot != nullptr && !def.integer)
            {
                const float scale = std::pow(10.0f, float(dot[1] - '0'));
                values[i] = std::round(values[i] * scale) / scale;
            }
            values[i] = std::clamp(values[i], def.min, def.max);
            save_value(section, def, values[i]);
            changed = true;
            char text[96];
            snprintf(text, sizeof(text), "%s: %s set to %g", title, def.label, values[i]);
            dd_log(reshade::log::level::info, text);
        }
        ImGui::EndDisabled();
        if (inactive != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", inactive);
        else if (def.tooltip != nullptr && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", def.tooltip);
    };
    // In the window's order, if it differs from the order of the definitions.
    for (int k = 0; k < count; ++k)
    {
        const int i = order != nullptr ? order[k] : k;
        if (!defs[i].advanced)
            slider(i);
    }
    ImGui::PopID();
    return changed;
}

void ao_section()
{
    if (tuning_section("Ambient occlusion", "AmbientOcclusion", AO_DEFS, &AO_PRESET_VALUES[0][0],
                       AO_PARAMS, g_ao, AO_ORDER))
    {
        ao_set(g_ao);
        scene_set(true);
    }
}

void ssr_section()
{
    if (tuning_section("Screen-space reflections (Water)", "ScreenSpaceReflectionsWater", SSR_DEFS,
                       &SSR_PRESET_VALUES[0][0], SSR_PARAMS, g_ssr, SSR_ORDER))
        ssr_set(g_ssr);
}

void surfaces_section()
{
    if (tuning_section("Screen-space reflections (Surfaces)", "ScreenSpaceReflectionsSurfaces", MAT_DEFS,
                       &MAT_PRESET_VALUES[0][0], MAT_PARAMS, g_mat, nullptr))
        scene_set(true);
}

void indirect_section()
{
    if (tuning_section("Indirect lighting", "IndirectLighting", SSIL_DEFS, &SSIL_PRESET_VALUES[0][0], SSIL_PARAMS,
                       g_ssil, SSIL_ORDER))
        scene_set(true);
}


// The status line above the settings: always exactly one short line, so the settings never move. Every message
// fits the window's default width. When several apply, the most important one shows: a part that is not running,
// then a load or the guard, then Running. The log has the details.
void draw_status()
{
    const float top = ImGui::GetCursorPosY();
    bool warning = false;
    std::string text;
    if (!g_enabled)
        text = "Off. Press " + (g_toggle_key != 0 ? key_name(g_toggle_key) : std::string("the toggle key")) +
               " or tick Enabled to switch it on.";
    else if (!lod_found())
        warning = true, text = "Level of detail not active. See acvfidelity.log.";
    else if (!ao_ready())
        warning = true, text = "Ambient occlusion not active yet.";
    else if (!ssr_ready())
        warning = true, text = "Reflections not active yet.";
    else if (lod_control_paused())
        text = "Loading: the game's own detail for a moment.";
    else if (lod_control_categories() && !lod_control_grass())
        text = "Grass distance: waiting for the game world.";
    else if (lod_control_guarded())
        text = "Level of detail eased while new detail streams in.";
    else
        text = "Running: level of detail, AO, reflections and indirect lighting.";
    if (warning)
        ImGui::PushStyleColor(ImGuiCol_Text, WARNING_COLOR);
    ImGui::TextUnformatted(text.c_str());
    if (warning)
        ImGui::PopStyleColor();
    ImGui::SetCursorPosY(top + ImGui::GetTextLineHeightWithSpacing());
}

// One preset for every section. It shows the preset all sections match, or Custom. Nothing of it is saved: it
// follows from the sections' values, like each section's own preset.
void global_preset()
{
    const int sections[] = {lod_preset(), preset_of(g_ao, &AO_PRESET_VALUES[0][0], AO_PARAMS),
                            preset_of(g_ssr, &SSR_PRESET_VALUES[0][0], SSR_PARAMS),
                            preset_of(g_mat, &MAT_PRESET_VALUES[0][0], MAT_PARAMS),
                            preset_of(g_ssil, &SSIL_PRESET_VALUES[0][0], SSIL_PARAMS)};
    int current = sections[0];
    for (int s : sections)
        if (s != current)
            current = PRESETS;
    ImGui::PushID("all");
    if (ImGui::BeginCombo("Preset", PRESET_NAMES[current]))
    {
        for (int p = 0; p < PRESETS; ++p)
            if (ImGui::Selectable(PRESET_NAMES[p], p == current) && p != current)
            {
                lod_apply_preset(p);
                std::copy(AO_PRESET_VALUES[p], AO_PRESET_VALUES[p] + AO_PARAMS, g_ao);
                std::copy(SSR_PRESET_VALUES[p], SSR_PRESET_VALUES[p] + SSR_PARAMS, g_ssr);
                std::copy(MAT_PRESET_VALUES[p], MAT_PRESET_VALUES[p] + MAT_PARAMS, g_mat);
                std::copy(SSIL_PRESET_VALUES[p], SSIL_PRESET_VALUES[p] + SSIL_PARAMS, g_ssil);
                save_section("AmbientOcclusion", AO_DEFS, g_ao, AO_PARAMS);
                save_section("ScreenSpaceReflectionsWater", SSR_DEFS, g_ssr, SSR_PARAMS);
                save_section("ScreenSpaceReflectionsSurfaces", MAT_DEFS, g_mat, MAT_PARAMS);
                save_section("IndirectLighting", SSIL_DEFS, g_ssil, SSIL_PARAMS);
                ao_set(g_ao);
                ssr_set(g_ssr);
                scene_set(true);
                char text[64];
                snprintf(text, sizeof(text), "Every section set to %s", PRESET_NAMES[p]);
                dd_log(reshade::log::level::info, text);
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sets every section below. Changing one afterward shows Custom.");
    ImGui::PopID();
}

void draw_window(effect_runtime *runtime)
{
    const float scale = ImGui::GetFontSize() / 13.0f;
    ImGui::SetWindowSize(ImVec2(560.0f * scale, 400.0f * scale), ImGuiCond_FirstUseEver);
    // The height follows the settings. Every one shows without scrolling. ImGui keeps it on the screen.
    ImGui::SetWindowSize(ImVec2(ImGui::GetWindowWidth(), 0.0f));

    draw_status();
    {
        bool enabled = g_enabled;
        if (ImGui::Checkbox("Enabled", &enabled))
            set_enabled(enabled, "the checkbox");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off gives the game's own graphics. Your settings are kept.");
        draw_toggle_key(runtime);
    }
    ImGui::BeginDisabled(!g_enabled);
    global_preset();
    ImGui::Spacing();
    lod_section();
    ImGui::Spacing();
    ao_section();
    ImGui::Spacing();
    ssr_section();
    ImGui::Spacing();
    surfaces_section();
    ImGui::Spacing();
    indirect_section();
    ImGui::EndDisabled();

}

// ReShade's Generic Depth add-on copies the game's depth buffer on ReShade's own command list before effects.
// It does this every frame, whether or not an effect uses it. During a fast travel or a load the game reuses that
// memory while the buffer still exists. The copy then faulted the GPU and the game closed, even with no other
// add-on. With copies before clear operations at full (2) it copies on the game's own command lists instead.
// The buffer is still valid there. It reads the setting when the device is made, after add-ons load. So it is
// set here when Generic Depth is on. ACV Fidelity doesn't need it. Turning it off works as well.
void guard_generic_depth()
{
    char disabled[2048] = {};
    size_t size = sizeof(disabled) - 1;
    if (reshade::get_config_value(nullptr, "ADDON", "DisabledAddons", disabled, &size))
    {
        // A list comes back as names separated by null characters.
        for (size_t at = 0; at < size && disabled[at] != '\0';)
        {
            const std::string name(disabled + at);
            if (name == "Generic Depth" || name.rfind("Generic Depth@", 0) == 0)
                return;
            at += name.size() + 1;
        }
    }
    int mode = 0;
    if (reshade::get_config_value(nullptr, "DEPTH", "DepthCopyBeforeClears", mode) && mode == 2)
        return;
    reshade::set_config_value(nullptr, "DEPTH", "DepthCopyBeforeClears", 2);
    char text[256];
    snprintf(text, sizeof(text),
             "ReShade's Generic Depth add-on is on: set its copy before clear operations to full (was %d). Its "
             "default crashed the game on fast travel. Turning Generic Depth off works as well",
             mode);
    dd_log(reshade::log::level::warning, text);
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;
        load_settings();
        diagnostics_start(config_float("Diagnostics", "Debug", 0.0f) != 0.0f);
        log_start();
        {
            char text[320];
            snprintf(text, sizeof(text),
                     "settings: detail distance %.2fx, objects %.2fx, foliage %.2fx, clutter %.2fx, grass %.2fx, AO %s, "
                     "reflections %s, surfaces %s, indirect lighting %s",
                     g_lod[0], g_lod[1 + LOD_OBJECTS], g_lod[1 + LOD_FOLIAGE], g_lod[1 + LOD_CLUTTER], g_lod[LOD_GRASS],
                     PRESET_NAMES[preset_of(g_ao, &AO_PRESET_VALUES[0][0], AO_PARAMS)],
                     PRESET_NAMES[preset_of(g_ssr, &SSR_PRESET_VALUES[0][0], SSR_PARAMS)],
                     PRESET_NAMES[preset_of(g_mat, &MAT_PRESET_VALUES[0][0], MAT_PARAMS)],
                     PRESET_NAMES[preset_of(g_ssil, &SSIL_PRESET_VALUES[0][0], SSIL_PARAMS)]);
            dd_log(reshade::log::level::info, text);
        }
        guard_generic_depth();
        push_lod(g_lod);
        reshade::register_event<reshade::addon_event::reshade_open_overlay>(on_overlay);
        reshade::register_event<reshade::addon_event::reshade_overlay>(on_overlay_draw);
        reshade::register_event<reshade::addon_event::reshade_present>(on_present);
        ao_set(g_ao);
        ao_register();
        ssr_set(g_ssr);
        ssr_register();
        // After light_register (in ao_register): the scene hook's pipeline bind runs after the lighting's, and
        // the state it reads is followed before it.
        scene_state::register_events();
        scene::register_events();
        ssr2_register();
        ssil_register();
        scene_set(true);
        if (!g_enabled)
            apply_all();
        reshade::register_overlay("ACV Fidelity", draw_window);
        break;
    case DLL_PROCESS_DETACH:
        // Unloaded while the game keeps running. Give it its own factor back.
        lod_control_shutdown();
        // At process exit the threads are already gone. Only a real unload shuts the modules down.
        if (reserved == nullptr)
        {
            ssr_unregister();
            diagnostics_stop();
            scene::unregister_events();
            scene_state::unregister_events();
            ao_unregister();
        }
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}
