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
    // Surface reflections: the space-2 register of our reflection texture, 0 for none. The probe reflection becomes
    // X + c (S / E - X) right after its CB2[0][88].w scale.
    uint32_t ssr2 = 0;
    // With ssr2: the variant also stores its own final gloss (the value its probe uses, after every class, snow and
    // puddle path) and 1 into our UAV at space-2 u0, at the pixel. Pixels no patched variant draws stay 0.
    bool ssr2_gloss = false;
    // Indirect lighting: the space-2 register of our indirect light texture, 0 for none. Right after the ambient
    // multiply (occlusion x ambient irradiance) the ambient becomes X - n_occ + n_occ min(M, V) / min(M, A) + rgb / E,
    // (rgb, a) read from that texture. n_occ is the game's bounce after its occlusion, copied where the game adds it.
    // The game's occlusion is min(M, A) (M the multi-bounce material AO, A the screen AO), times a class factor in
    // some variants; the ratio keeps that factor. Our visibility V = a - 1 takes the place of A. Where that min is not
    // found the ambient gets rgb / E only. a = 0 where we have nothing, which keeps X. The ambient gain after the site
    // does not scale ours (light_patch.cpp gain_edits).
    uint32_t ssil = 0;
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
    const char *ssr2_missing = nullptr; // why the reflection patch can't go in (checked with register 14)
    const char *ssil_missing = nullptr; // why the indirect light patch can't go in (checked with register 15)
    bool ssr2_in = false, ssil_in = false; // what light_patch put in
    bool bounce_copy_in = false;   // the bounce term is copied where the game adds it
    bool bounce_rewritten = false; // the ambient register is written again between that add and the site
    uint32_t bounce_reg = 0;       // the register the bounce add writes (one operand of the ambient mul)
    const char *bounce_missing = nullptr; // why the bounce copy is not in
    bool material_min_in = false;  // the occlusion min (M, screen AO) found for the combine
    bool gain_in = false;          // ours is kept out of the ambient gain after the site
};

// Space-2 t14 is inside the lighting's space-2 table (t0..t21) and declared by none of the 541 known variants.
const uint32_t LIGHT_SSR2_REGISTER = 14;
// Indirect lighting: declared by none of the 541 known variants either.
const uint32_t LIGHT_SSIL_REGISTER = 15;
// Space-2 u0: the lighting's root signature has it in the space-2 table; no variant declares any UAV.
const uint32_t LIGHT_SSR2_GLOSS_UAV = 0;
// light_support::ssr2_missing for a variant with no probe reflection at all (nothing to replace).
const char *const LIGHT_SSR2_NO_PROBE = "no reflection to replace";

// Both hallmark literals of the lighting's multi-bounce AO curve are present.
bool light_is_candidate(const uint8_t *code, size_t size);
bool light_changes(const light_values &v);
light_support light_check(const uint8_t *code, size_t size);
// The patched copy, checksum included. False when a check fails.
bool light_patch(const uint8_t *code, size_t size, const light_values &v, std::vector<uint8_t> &out);
// What light_patch puts in with these values (the flags of light_support), without building the code.
light_support light_check_with(const uint8_t *code, size_t size, const light_values &v);
