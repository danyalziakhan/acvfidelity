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
#include "log.h"
#include "lod.h"
#include "lod_control.h"
#include "lod_hook.h"
#include "ssr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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
// geometry glitches and crashes in testing. The categories stop at 2.0x on top of it, as tested in game.
const float MAX_DISTANCE = 2.5f;
const float MAX_CATEGORY = 2.0f;
// Clutter only covers small plants near the player. Their spawn radius stays fixed. So it can go further.
const float MAX_CLUTTER = 4.0f;
// Grass: above 3x (about 250 m) plants blinked and stretched in testing and the GPU hung. More clutter made it
// worse. That showed in the raven view zoomed out, where the most grass cells are in view.
const float MAX_GRASS = 3.0f;
// The window's level of detail values: detail distance, the categories, then grass.
const int LOD_GRASS = 1 + LOD_CATEGORIES;
const int LOD_VALUES = 2 + LOD_CATEGORIES;
// Game, Fidelity I, Fidelity II, Fidelity III: detail distance, then objects, clutter, foliage and grass.
const float LOD_PRESETS[PRESETS][LOD_VALUES] = {
    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {1.5f, 1.25f, 2.0f, 1.25f, 1.67f},
    {2.0f, 1.5f, 3.0f, 1.5f, 2.33f},
    {2.5f, 2.0f, 4.0f, 2.0f, 3.0f},
};
const char *const LOD_KEYS[LOD_VALUES] = {"DetailDistance", "ObjectsDistance", "ClutterDetail", "FoliageDistance",
                                          "GrassDistance"};

// The window's level of detail values, in the order of LOD_PRESETS.
float g_lod[LOD_VALUES] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
float g_ao[AO_PARAMS];
float g_ssr[SSR_PARAMS];


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

void load_settings()
{
    config_open();
    for (int i = 0; i < LOD_VALUES; ++i)
    {
        const float v = config_float("LevelOfDetail", LOD_KEYS[i], LOD_PRESETS[2][i]);
        const float max = i == 0                 ? MAX_DISTANCE
                          : i == 1 + LOD_CLUTTER ? MAX_CLUTTER
                          : i == LOD_GRASS       ? MAX_GRASS
                                                 : MAX_CATEGORY;
        g_lod[i] = std::isfinite(v) ? std::clamp(v, 1.0f, max) : LOD_PRESETS[2][i];
    }
    load_values("AmbientOcclusion", AO_DEFS, &AO_PRESET_VALUES[0][0], AO_PARAMS, AO_DEFAULT_PRESET, g_ao);
    load_values("ScreenSpaceReflections", SSR_DEFS, &SSR_PRESET_VALUES[0][0], SSR_PARAMS, SSR_DEFAULT_PRESET, g_ssr);
}


// Hands the values to the LOD control, which eases the game toward them.
void push_lod(const float *values)
{
    lod_control_set(values[0], values + 1, values[LOD_GRASS]);
}

void save_lod()
{
    for (int i = 0; i < LOD_VALUES; ++i)
        config_set("LevelOfDetail", LOD_KEYS[i], std::round(g_lod[i] * 100.0f) / 100.0f);
}





void on_present(effect_runtime *runtime)
{
    diagnostics_present(runtime->get_command_queue());
    lod_control_frame();
}

const ImVec4 WARNING_COLOR(0.96f, 0.73f, 0.24f, 1.0f);

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

void lod_section()
{
    ImGui::SeparatorText("Level of detail");
    if (!lod_found())
        return;
    ImGui::PushID("lod");
    int current = PRESETS;
    for (int p = 0; p < PRESETS; ++p)
    {
        bool same = true;
        for (int i = 0; i < LOD_VALUES; ++i)
            same &= std::fabs(g_lod[i] - LOD_PRESETS[p][i]) < 0.005f;
        if (same)
            current = p;
    }
    bool changed = false;
    if (ImGui::BeginCombo("Preset", PRESET_NAMES[current]))
    {
        for (int p = 0; p < PRESETS; ++p)
            if (ImGui::Selectable(PRESET_NAMES[p], p == current) && p != current)
            {
                std::copy(LOD_PRESETS[p], LOD_PRESETS[p] + LOD_VALUES, g_lod);
                changed = true;
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
        changed |= lod_slider(1 + LOD_OBJECTS, "Objects distance", MAX_CATEGORY,
                              "Rocks, props, bushes and some trees on top of the detail distance.\n"
                              "1.0x is the game's own.");
        changed |= lod_slider(1 + LOD_FOLIAGE, "Foliage distance", MAX_CATEGORY,
                              "Leaves and branches of trees and large plants on top of the\n"
                              "detail distance. 1.0x is the game's own.");
        changed |= lod_slider(1 + LOD_CLUTTER, "Clutter detail", MAX_CLUTTER,
                              "Detail of the small plants and clutter near you.\n1.0x is the game's own.");
        changed |= lod_slider(LOD_GRASS, "Grass distance", MAX_GRASS,
                              "How far out grass, flowers and small plants appear.\n1.0x is the game's own.");
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
bool tuning_section(const char *title, const char *section, const param_def *defs, const float *presets,
                    int count, float *values, const int *order = nullptr)
{
    ImGui::SeparatorText(title);
    if (strcmp(section, "ScreenSpaceReflections") == 0)
    {
        ImGui::TextDisabled("Water and other flat shiny surfaces.");
        ImGui::Spacing();
    }
    ImGui::PushID(title);
    bool changed = false;
    auto save_all = [&] {
        for (int i = 0; i < count; ++i)
            if (!defs[i].advanced)
                save_value(section, defs[i], values[i]);
    };
    const int current = preset_of(values, presets, count);
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
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sets every value below. Changing one afterward shows Custom.");

    auto slider = [&](int i) {
        const param_def &def = defs[i];
        ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
        if (def.logarithmic)
            flags |= ImGuiSliderFlags_Logarithmic;
        ImGui::SliderFloat(def.label, &values[i], def.min, def.max, def.format, flags);
        if (def.integer)
            values[i] = std::round(values[i]);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            // To the decimals shown, so the ini holds what the window showed and presets match again.
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
        if (def.tooltip != nullptr && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", def.tooltip);
    };
    // The window's order, if it differs from the order of the definitions.
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
    if (tuning_section("Ambient occlusion", "AmbientOcclusion", AO_DEFS, &AO_PRESET_VALUES[0][0], AO_PARAMS, g_ao))
        ao_set(g_ao);
}

void ssr_section()
{
    if (tuning_section("Screen-space reflections", "ScreenSpaceReflections", SSR_DEFS, &SSR_PRESET_VALUES[0][0], SSR_PARAMS,
                       g_ssr, SSR_ORDER))
        ssr_set(g_ssr);
}


// The status lines above the settings. At most one line each for level of detail, ambient occlusion and
// reflections. The area keeps that height so the settings don't move when a line comes or goes.
const int STATUS_LINES = 3;

void draw_status()
{
    const float top = ImGui::GetCursorPosY();
    bool any = false;
    const auto line = [&](bool warning, const std::string &text) {
        if (warning)
            ImGui::PushStyleColor(ImGuiCol_Text, WARNING_COLOR);
        ImGui::TextWrapped("%s", text.c_str());
        if (warning)
            ImGui::PopStyleColor();
        any = true;
    };
    if (!lod_found())
        line(true, "Level of detail not active: " + lod_where() + ".");
    else if (lod_control_paused())
        line(false, "Loading: the game's own detail for a moment.");
    else if (lod_control_categories() && !lod_control_grass())
        line(false, "Grass distance: waiting for the game world.");
    if (!ao_ready())
        line(true, "Ambient occlusion not active yet: the game's AO shader was not seen.");
    if (!ssr_ready())
        line(true, "Reflections not active yet: the game's reflection shader was not seen.");
    if (!any)
        line(false, "Running: level of detail, ambient occlusion and reflections.");
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), top + STATUS_LINES * ImGui::GetTextLineHeightWithSpacing()));
}

void draw_window(effect_runtime *)
{
    const float scale = ImGui::GetFontSize() / 13.0f;
    ImGui::SetWindowSize(ImVec2(560.0f * scale, 400.0f * scale), ImGuiCond_FirstUseEver);

    draw_status();
    lod_section();
    ImGui::Spacing();
    ao_section();
    ImGui::Spacing();
    ssr_section();

}

// ReShade's Generic Depth add-on copies the game's depth buffer on ReShade's own command list before effects.
// It does this every frame, whether or not an effect uses it. During a fast travel or a load the game reuses that
// memory while the buffer still exists. The copy then faulted the GPU and the game closed, even with no other
// add-on. With copies before clear operations at full (2) it copies on the game's own command lists instead.
// The buffer is still valid there. It reads the setting when the device is made, after add-ons load. So it is
// set here when Generic Depth is on. ACV Fidelity doesn't need it, so turning it off works as well.
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
            char text[192];
            snprintf(text, sizeof(text),
                     "settings: detail distance %.2fx, objects %.2fx, foliage %.2fx, clutter %.2fx, grass %.2fx, AO %s, "
                     "reflections %s",
                     g_lod[0], g_lod[1 + LOD_OBJECTS], g_lod[1 + LOD_FOLIAGE], g_lod[1 + LOD_CLUTTER], g_lod[LOD_GRASS],
                     PRESET_NAMES[preset_of(g_ao, &AO_PRESET_VALUES[0][0], AO_PARAMS)],
                     PRESET_NAMES[preset_of(g_ssr, &SSR_PRESET_VALUES[0][0], SSR_PARAMS)]);
            dd_log(reshade::log::level::info, text);
        }
        guard_generic_depth();
        push_lod(g_lod);
        reshade::register_event<reshade::addon_event::reshade_present>(on_present);
        ao_set(g_ao);
        ao_register();
        ssr_set(g_ssr);
        ssr_register();
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
            ao_unregister();
        }
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}
