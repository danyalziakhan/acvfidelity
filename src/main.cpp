// ACV Fidelity
//
// Takes Assassin's Creed Valhalla's graphics past its own highest settings.
// Ambient occlusion is in ao.cpp and reflections in ssr.cpp. Level of detail lowers the game's LOD factor
// (lod.cpp) to keep the detailed models on screen farther away.

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

// The window shows the inverse of the factor as the detail distance. It stops at 2.5x (factor 0.4).
// Nothing more was visible past it. Lower factors brought geometry glitches and crashes in testing.
const float GAME_FACTOR = 1.0f;
const float MAX_DISTANCE = 2.5f;
// Game, Fidelity I, Fidelity II, Fidelity III.
const float DISTANCE_PRESETS[PRESETS] = {1.0f, 1.5f, 2.0f, 2.5f};


float g_factor = GAME_FACTOR; // 1 / the detail distance slider
float g_distance = 1.0f;     // the slider
float g_ao[AO_PARAMS];
float g_ssr[SSR_PARAMS];

float g_written = GAME_FACTOR;

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
    g_distance = std::clamp(config_float("LevelOfDetail", "DetailDistance", DISTANCE_PRESETS[2]), 1.0f, MAX_DISTANCE);
    load_values("AmbientOcclusion", AO_DEFS, &AO_PRESET_VALUES[0][0], AO_PARAMS, AO_DEFAULT_PRESET, g_ao);
    load_values("ScreenSpaceReflections", SSR_DEFS, &SSR_PRESET_VALUES[0][0], SSR_PARAMS, SSR_DEFAULT_PRESET, g_ssr);
    g_factor = 1.0f / g_distance;
}


void save_distance()
{
    config_set("LevelOfDetail", "DetailDistance", std::round(g_distance * 100.0f) / 100.0f);
}

float effective_factor()
{
    return g_factor;
}

void apply_factor()
{
    const float factor = effective_factor();
    if (factor == g_written)
        return;
    lod_write(factor);
    g_written = factor;
}





void on_present(effect_runtime *runtime)
{
    diagnostics_present(runtime->get_command_queue());
    if (!lod_found() && !lod_find())
        return;
    apply_factor();
}

const ImVec4 WARNING_COLOR(0.96f, 0.73f, 0.24f, 1.0f);

void set_distance(float distance)
{
    g_distance = std::clamp(distance, 1.0f, MAX_DISTANCE);
    g_factor = 1.0f / g_distance;
    save_distance();
    apply_factor();
}

void lod_section()
{
    ImGui::SeparatorText("Level of detail");
    if (!lod_found())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, WARNING_COLOR);
        ImGui::TextWrapped("Not active: %s.", lod_where().c_str());
        ImGui::PopStyleColor();
        return;
    }


    int current = PRESETS;
    for (int p = 0; p < PRESETS; ++p)
        if (std::fabs(g_distance - DISTANCE_PRESETS[p]) < 0.005f)
            current = p;
    if (ImGui::BeginCombo("Preset", PRESET_NAMES[current]))
    {
        for (int p = 0; p < PRESETS; ++p)
            if (ImGui::Selectable(PRESET_NAMES[p], p == current) && p != current)
            {
                set_distance(DISTANCE_PRESETS[p]);
                char text[64];
                snprintf(text, sizeof(text), "Level of detail set to %s", PRESET_NAMES[p]);
                dd_log(reshade::log::level::info, text);
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sets the detail distance below. Each preset reaches farther.");

    float distance = g_distance;
    ImGui::SliderFloat("Detail distance", &distance, 1.0f, MAX_DISTANCE, "%.2fx",
                       ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    if (distance != g_distance)
        set_distance(std::round(distance * 20.0f) / 20.0f);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        char text[48];
        snprintf(text, sizeof(text), "Detail distance set to %.2fx", g_distance);
        dd_log(reshade::log::level::info, text);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far away objects keep their detailed models.\n"
                          "1.0x is the game's own.");

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
                    int count, float *values)
{
    ImGui::SeparatorText(title);
    if (strcmp(section, "ScreenSpaceReflections") == 0)
        ImGui::TextDisabled("Water and other flat shiny surfaces.");
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
    for (int i = 0; i < count; ++i)
        if (!defs[i].advanced)
            slider(i);
    ImGui::PopID();
    return changed;
}

void ao_section()
{
    if (tuning_section("Ambient occlusion", "AmbientOcclusion", AO_DEFS, &AO_PRESET_VALUES[0][0], AO_PARAMS, g_ao))
        ao_set(g_ao);
    if (!ao_ready())
        ImGui::TextColored(WARNING_COLOR, "Not active yet: the game's AO shader was not seen.");
}

void ssr_section()
{
    if (tuning_section("Screen-space reflections", "ScreenSpaceReflections", SSR_DEFS, &SSR_PRESET_VALUES[0][0], SSR_PARAMS, g_ssr))
        ssr_set(g_ssr);
    if (!ssr_ready())
        ImGui::TextColored(WARNING_COLOR, "Not active yet: the game's reflection shader was not seen.");
}

void draw_window(effect_runtime *)
{
    const float scale = ImGui::GetFontSize() / 13.0f;
    ImGui::SetWindowSize(ImVec2(560.0f * scale, 400.0f * scale), ImGuiCond_FirstUseEver);

    lod_section();
    ImGui::Spacing();
    ao_section();
    ImGui::Spacing();
    ssr_section();

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
            snprintf(text, sizeof(text), "settings: detail distance %.2fx, AO %s, reflections %s", g_distance,
                     PRESET_NAMES[preset_of(g_ao, &AO_PRESET_VALUES[0][0], AO_PARAMS)],
                     PRESET_NAMES[preset_of(g_ssr, &SSR_PRESET_VALUES[0][0], SSR_PARAMS)]);
            dd_log(reshade::log::level::info, text);
        }
        if (lod_find())
            apply_factor();
        reshade::register_event<reshade::addon_event::reshade_present>(on_present);
        ao_set(g_ao);
        ao_register();
        ssr_set(g_ssr);
        ssr_register();
        reshade::register_overlay("ACV Fidelity", draw_window);
        break;
    case DLL_PROCESS_DETACH:
        // Unloaded while the game keeps running. Give it its own factor back.
        if (lod_found() && g_written != GAME_FACTOR)
            lod_write(GAME_FACTOR);
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
