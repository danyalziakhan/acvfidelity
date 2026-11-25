// Screen-space reflections for the scene lighting's glossy surfaces.
//
// Based on AMD FidelityFX SSSR (Stochastic Screen Space Reflections, MIT license, Copyright (c) 2020 Advanced Micro
// Devices, Inc.): the hierarchical depth traversal (step to the next cell border, go up a level when a whole cell is
// skipped, down when the ray reaches the surface), one GGX-sampled ray per pixel, temporal accumulation and a
// spatial resolve. Written for this add-on; no FidelityFX source is included.
//
// Roughness: the game's own sun specular uses GGX with alpha^2 = (1 - g)^4 (5859ADE677D4FDF1 lines 672-674 and
// 766-770, g = s1 t8.y). So roughness r = 1 - g and alpha = r^2 here too: our lobe matches the game's highlight.
//
// Output of the final pass (read by the patched lighting at space-2 t14): rgb = reflected colour in this frame's
// exposed units (the lighting divides by its exposure), a = confidence x strength. A miss gives a = 0, so the
// game's probe reflection stays.

#include "common.hlsli"

Texture2D<float> Pyramid : register(t0);      // forward depth, all mips
Texture2D<float4> Normals : register(t1);     // world normal, w roughness
Texture2D<float> LinearDepth : register(t2);  // meters, mip 1 = half resolution
Texture2D<float4> Capture : register(t3);     // last frame's lit colour, exposed units, mips
Texture2D<float4> GameAlbedo : register(t4);  // s1 t6, w material id / 255
Texture2D<float4> GameMaterial : register(t5); // s1 t8, w separates grass from cloth in class 15
Texture2D<float2> Motion : register(t6);      // the shared motion (trace only; t6 is the resolve's input later)
StructuredBuffer<float4> Cam : register(t7);

RWTexture2D<float4> Out : register(u0);

#define FLAG_FINAL 16u // the pass writes the texture the lighting reads
#define FLAG_CHARACTERS 128u      // character surfaces (cloth, armour, fur: class 15 with low t8.w) get ours too
#define FLAG_NO_CHARACTER_HITS 256u // a ray that ends on a character counts as a miss (the probe stays)

// A character's surface by the ids one view showed (skin 23, beard 24, hair 27, class 15 with low t8.w). Ids are
// not fixed materials across views: used only for the Characters and CharacterHits switches, never for the
// reflects mask (that comes from the game's lighting, see CSPrepare).
bool character(uint2 full)
{
    const uint id = (uint)round(GameAlbedo.Load(int3(full, 0)).w * 255.0);
    return id == 23 || id == 24 || id == 27 || (id == 15 && GameMaterial.Load(int3(full, 0)).w <= 0.8);
}

// No reflection of ours here: the reflects mask (roughness -1: no patched lighting variant drew the pixel last
// frame), or a character with the Characters switch off.
bool excluded(uint2 full)
{
    return Normals.Load(int3(full, 0)).w < 0.0 || ((P2.y & FLAG_CHARACTERS) == 0 && character(full));
}

// GGX visible normal sample (Heitz 2018) in the frame of n.
float3 sample_ggx(float3 to_eye, float3 n, float alpha, float2 u)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z), b = n.x * n.y * a;
    const float3 t = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    const float3 bt = float3(b, s + n.y * n.y * a, -n.y);
    const float3 v = float3(dot(to_eye, t), dot(to_eye, bt), dot(to_eye, n));
    const float3 vh = normalize(float3(alpha * v.x, alpha * v.y, max(v.z, 1e-4)));
    const float len2 = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = len2 > 0.0 ? float3(-vh.y, vh.x, 0.0) * rsqrt(len2) : float3(1.0, 0.0, 0.0);
    const float3 t2 = cross(vh, t1);
    const float r = sqrt(u.x), phi = 6.2831853 * u.y;
    const float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    const float k = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - k) * sqrt(1.0 - p1 * p1) + k * p2;
    const float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    const float3 h = normalize(float3(alpha * nh.x, alpha * nh.y, max(nh.z, 0.0)));
    return h.x * t + h.y * bt + h.z * n;
}

float2 mip_size(float2 size, int mip)
{
    return max(floor(size / exp2((float)mip)), 1.0);
}

// The hierarchical march over the forward-depth pyramid, in (uv, forward depth). Returns the end point. `hit` is
// true when the ray reached a surface at mip 0 within the step budget and on screen.
float3 march(float3 origin, float3 dir, float2 size, uint steps, uint mips, out bool hit)
{
    const float big = 3.402823e38;
    const float3 inv = float3(dir.x != 0.0 ? 1.0 / dir.x : big, dir.y != 0.0 ? 1.0 / dir.y : big,
                              dir.z != 0.0 ? 1.0 / dir.z : big);
    const float2 floor_offset = float2(dir.x < 0.0 ? 0.0 : 1.0, dir.y < 0.0 ? 0.0 : 1.0);
    const float2 uv_offset = float2(dir.x < 0.0 ? -0.005 : 0.005, dir.y < 0.0 ? -0.005 : 0.005) / size;

    // First step: to the border of the start texel, so the ray does not hit its own surface.
    float2 res = size;
    const float2 border = (floor(origin.xy * res) + floor_offset) / res + uv_offset;
    const float2 tb = border * inv.xy - origin.xy * inv.xy;
    float t = min(tb.x, tb.y);
    float3 p = origin + t * dir;

    int mip = 0;
    uint i = 0;
    while (i < steps && mip >= 0)
    {
        if (any(p.xy < 0.0) || any(p.xy > 1.0))
            break;
        const float2 cell = p.xy * res;
        const float surface = Pyramid.Load(int3(cell, mip));
        const float2 xy = (floor(cell) + floor_offset) / res + uv_offset;
        float3 tp = float3(xy, surface) * inv - origin * inv;
        tp.z = dir.z > 0.0 ? tp.z : big;
        const float tmin = min(min(tp.x, tp.y), tp.z);
        const bool above = surface > p.z;
        const bool skipped = asuint(tmin) != asuint(tp.z) && above;
        t = above ? tmin : t;
        p = origin + t * dir;
        mip = skipped ? min(mip + 1, (int)mips - 1) : mip - 1;
        res = mip_size(size, max(mip, 0));
        ++i;
    }
    hit = mip < 0 && all(p.xy >= 0.0) && all(p.xy <= 1.0);
    return p;
}

// ---------------------------------------------------------------- trace
// P0.xy trace size, P0.zw full size. P1: x max roughness, y thickness (m), z roughness fade width, w lobe trim.
// P2: x frame, y flags, z pyramid mips, w steps. P3: x luminance cap (exposed units), y/z motion fade (px).

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void CSTrace(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.xy))
        return;
    const float2 size = float2(P0.zw);
    const uint2 full = min(uint2(id.xy * size / float2(P0.xy)), P0.zw - 1);
    const float4 nr = Normals.Load(int3(full, 0));
    const float rough = nr.w;
    const float fade = saturate((P1.x - rough) / max(P1.z, 1e-3));
    // After a camera cut the capture shows the view before it: nothing of ours this frame.
    if (fade <= 0.0 || (P2.y & FLAG_CAPTURE) == 0 || Cam[CAM_INFO].y > 0.5 || excluded(full))
    {
        Out[id.xy] = 0.0;
        return;
    }
    const bool reversed = Cam[CAM_INFO].x > 0.5;
    const float4x4 inv = rows(Cam, CAM_INV_VP), vp = rows(Cam, CAM_VP);
    const float2 uv = (full + 0.5) / size;
    const float fd = Pyramid.Load(int3(full, 0));
    const float3 pos = position_of(inv, uv, device_depth(fd, reversed));
    // Moving things (characters, animals, cloth in wind): the game's motion differs from what the camera alone
    // gives. Ours flickered on wet armour there. They fade to the game's probe reflection:
    // fully ours below 1 px of own motion, fully the probe from 4 px, a smooth blend between (no switching when a
    // character starts or stops moving; the temporal pass smooths it further). 1 px leaves room for jitter.
    float still = 1.0;
    {
        const reprojected cam_only = reproject(Cam, pos);
        const float2 moved = (previous_uv(Motion, full, uv) - cam_only.uv) * size;
        if (cam_only.inside && P3.z > 0.0)
            still = 1.0 - smoothstep(P3.y, max(P3.z, P3.y + 1e-3), length(moved));
        if (still <= 0.0)
        {
            Out[id.xy] = 0.0;
            return;
        }
    }
    const float3 v = normalize(pos);
    const float3 n = nr.xyz;

    float2 u = random2(id.xy, P2.x);
    u.x *= P1.w; // keep to the core of the lobe; the spatial and temporal passes blur the rest
    const float3 h = sample_ggx(-v, n, rough * rough, u);
    float3 r = reflect(v, h);
    if (dot(r, n) <= 0.0)
        r = reflect(v, n);

    // The ray in (uv, forward depth). A point along it must stay in front of the camera.
    float len = 0.25 * length(pos) + 0.1;
    float4 end = project(vp, pos + r * len, reversed);
    [unroll] for (int k = 0; k < 6 && end.w <= 1e-3; ++k)
    {
        len *= 0.25;
        end = project(vp, pos + r * len, reversed);
    }
    float4 result = 0.0;
    if (end.w > 1e-3)
    {
        const float3 origin = float3(uv, fd);
        bool hit;
        const float3 p = march(origin, end.xyz - origin, size, P2.w, P2.z, hit);
        const uint2 at = min(uint2(p.xy * size), P0.zw - 1);
        const float surface = Pyramid.Load(int3(at, 0));
        hit = hit && surface < 0.999999;
        // Thickness: a ray that passes far behind the surface it ends on went under it.
        const float3 on_surface = position_of(inv, p.xy, device_depth(surface, reversed));
        const float3 on_ray = position_of(inv, p.xy, device_depth(p.z, reversed));
        const float behind = length(on_ray) - length(on_surface);
        hit = hit && behind < P1.y * (1.0 + 0.02 * length(on_surface));
        // Not its own pixel, not the back of a surface.
        hit = hit && length((p.xy - uv) * size) >= 1.5;
        hit = hit && dot(Normals.Load(int3(at, 0)).xyz, r) < 0.1;
        // Rays ending on a character (the reflecting character itself included).
        const bool on_character = hit && character(at);
        hit = hit && !(on_character && (P2.y & FLAG_NO_CHARACTER_HITS) != 0);
        if (hit)
        {
            // Where the hit surface was last frame: the shared motion at the hit pixel.
            const float2 before = previous_uv(Motion, at, p.xy);
            if (all(before >= 0.0) && all(before <= 1.0))
            {
                const float2 edge = min(p.xy, 1.0 - p.xy);
                const float confidence = still * fade * saturate(min(edge.x, edge.y) / 0.05);
                // Wider reflections read a blurrier mip of the capture.
                const float pixels = length((p.xy - uv) * size);
                const float mip = clamp(log2(max(rough * pixels * 0.5, 1.0)), 0.0, 4.0);
                float3 c = Capture.SampleLevel(LinearClamp, before, mip).rgb * Cam[CAM_INFO].w;
                const float l = luminance(c);
                c *= l > P3.x ? P3.x / l : 1.0;
                result = float4(c * confidence, confidence); // premultiplied: misses filter as nothing
            }
        }
    }
    Out[id.xy] = result;
}

// ---------------------------------------------------------------- spatial resolve
// t6 the temporal result (premultiplied). P0.xy pass size, P0.zw full size, P1.x radius in pixels at roughness 1,
// P2.z linear depth mip, P3.x strength. With FLAG_FINAL it writes the lighting's texture, else premultiplied.

Texture2D<float4> Accumulated : register(t6);

float4 finish(float4 v, float2 uv, uint2 full)
{
    const bool final_write = (P2.y & FLAG_FINAL) != 0;
    // Pixels without our reflection get exactly 0 at the last write. The trace writes 0 there, but the temporal pass
    // (3x3 clamp, bilinear history), the spatial pass (8 taps into a centre of 0) and the upsample (4 taps) carry
    // neighbours' values into them.
    const bool drop = excluded(full);
    // Seam guard: where our confidence is below 1/1024 (or the value is broken) the texture holds exactly (0, 0, 0, 0).
    // The lighting's P + c (S / E - P) is then P + 0 x (0 - P) = P, bit for bit the game's probe reflection.
    const bool bad = any((asuint(v) & 0x7F800000u) == 0x7F800000u);
    const float c = !bad && !drop && v.a >= 1.0 / 1024.0 ? saturate(v.a) : 0.0;
    const float3 s = c > 0.0 ? v.rgb / v.a : 0.0;
    return final_write ? (c > 0.0 ? float4(s, c * P3.x) : 0.0) : v;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void CSSpatial(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.xy))
        return;
    const float2 scale = float2(P0.zw) / float2(P0.xy);
    const uint2 full = min(uint2(id.xy * scale), P0.zw - 1);
    const float4 nr = Normals.Load(int3(full, 0));
    // P2.z 0 below full size (three quarters): no depth mip matches, read full resolution at the mapped pixel.
    const float d = LinearDepth.Load(int3(P2.z == 0 ? full : id.xy, P2.z));
    float4 sum = Accumulated[id.xy];
    const float radius = P1.x * saturate(nr.w) / scale.x;
    if (nr.w >= 0.05 && radius >= 0.5 && d < 1e5)
    {
        static const float2 DISC[8] = {float2(0.527, -0.085), float2(-0.040, 0.536), float2(-0.670, -0.179),
                                       float2(0.419, 0.616), float2(0.440, -0.639), float2(-0.757, 0.349),
                                       float2(0.574, 0.684), float2(0.033, -0.997)};
        const float angle = random2(id.xy, P2.x).x * 6.2831853;
        const float2x2 turn = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
        float weight = 1.0;
        [unroll] for (int k = 0; k < 8; ++k)
        {
            const int2 q = clamp(int2(id.xy) + int2(round(mul(turn, DISC[k]) * radius)), 0, int2(P0.xy) - 1);
            const uint2 qf = min(uint2(q * scale), P0.zw - 1);
            const float dq = LinearDepth.Load(int3(P2.z == 0 ? int2(qf) : q, P2.z));
            const float4 nq = Normals.Load(int3(qf, 0));
            // Only from pixels that reflect themselves.
            const float w = nq.w < 0.0 ? 0.0 : exp(-abs(dq - d) / (0.02 * d + 0.01)) * pow(saturate(dot(nr.xyz, nq.xyz)), 32.0);
            sum += Accumulated[q] * w;
            weight += w;
        }
        sum /= weight;
    }
    Out[id.xy] = finish(sum, (full + 0.5) / float2(P0.zw), full);
}

// ---------------------------------------------------------------- upsample to full resolution
// t6 the reduced resolve (premultiplied). P0.xy full size, P0.zw reduced size, P2.z its depth mip (1 at half,
// 0 at three quarters: full-resolution depth at the mapped pixel), P3.x strength.

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void CSUpsample(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.xy))
        return;
    const float d = LinearDepth.Load(int3(id.xy, 0));
    const float2 low = (id.xy + 0.5) * float2(P0.zw) / float2(P0.xy) - 0.5;
    const int2 base = int2(floor(low));
    const float2 f = low - base;
    float4 sum = 0.0;
    float weight = 0.0;
    [unroll] for (int y = 0; y < 2; ++y)
        [unroll] for (int x = 0; x < 2; ++x)
        {
            const int2 q = clamp(base + int2(x, y), 0, int2(P0.zw) - 1);
            const float bilinear = (x ? f.x : 1.0 - f.x) * (y ? f.y : 1.0 - f.y);
            const int2 qd = P2.z == 0 ? int2(min(uint2((q + 0.5) * float2(P0.xy) / float2(P0.zw)), P0.xy - 1)) : q;
            const float w = bilinear * exp(-abs(LinearDepth.Load(int3(qd, P2.z)) - d) / (0.02 * d + 0.01)) + 1e-5;
            sum += Accumulated[q] * w;
            weight += w;
        }
    // finish() reads the capture at full resolution here.
    Out[id.xy] = finish(sum / weight, (id.xy + 0.5) / float2(P0.xy), id.xy);
}
