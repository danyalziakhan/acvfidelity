#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The game's scene lighting is a family of full-screen pixel shaders, one per
// tile class, 528 of them. They are patched by pattern, not by offset. Only a
// shader that passes every check is changed.

struct light_values
{
    float blend = 0.0f;     // baked shade blend, the game's 0
    float sun = 0.0f;       // sunlight occlusion, the game's 0
    float sun_power = 3.0f; // the AO's power in sunlight occlusion
    float bounce = 1.0f;    // the game's 1
    float sky = 1.0f;       // sky light by day, the game's 1
    float sky_night = 1.0f; // sky light at night, the game's 1
    float contact = 1.0f;   // the game's 1
};

// What a shader supports. With `failed` set nothing is patched.
struct light_support
{
    const char *failed = nullptr;
    int min_sites = 0;
    bool sun_light = false;            // false: the variant has no sun light at all
    int sun_path = 0;                  // 1: after the shadow, NdotL and AO; 2: at the sun color; 3: without NdotL
    const char *sun_missing = nullptr; // why sunlight occlusion can't go in, when there is sun light
    bool bounce = false, sky = false, contact = false;
    uint32_t ao_register = 0;
    char ao_component = 0;
};

// Both hallmark literals of the lighting's multi-bounce AO curve are present.
bool light_is_candidate(const uint8_t *code, size_t size);
bool light_changes(const light_values &v);
light_support light_check(const uint8_t *code, size_t size);
// The patched copy, checksum included. False when a check fails.
bool light_patch(const uint8_t *code, size_t size, const light_values &v, std::vector<uint8_t> &out);
