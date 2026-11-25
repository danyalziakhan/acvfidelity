// The scene hook's shared passes: camera block, linear depth (full and a plain half-res mip), decoded
// normals, the forward-depth pyramid, the lit colour capture with mips, and the one temporal accumulation shader.
// Compiled by fxc at build time (cs_5_1) into headers that scene_inject.cpp embeds.

#include "common.hlsli"

// ---------------------------------------------------------------- setup: one thread

StructuredBuffer<float> Exposure : register(t0);
RWStructuredBuffer<float4> CamOut : register(u0);

float4x4 inverse4(float4x4 m)
{
    const float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2], a03 = m[0][3];
    const float a10 = m[1][0], a11 = m[1][1], a12 = m[1][2], a13 = m[1][3];
    const float a20 = m[2][0], a21 = m[2][1], a22 = m[2][2], a23 = m[2][3];
    const float a30 = m[3][0], a31 = m[3][1], a32 = m[3][2], a33 = m[3][3];
    const float b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10;
    const float b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
    const float b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30;
    const float b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
    const float det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    const float k = abs(det) > 1e-30 ? 1.0 / det : 0.0;
    return k * float4x4(
        a11 * b11 - a12 * b10 + a13 * b09, a02 * b10 - a01 * b11 - a03 * b09, a31 * b05 - a32 * b04 + a33 * b03,
        a22 * b04 - a21 * b05 - a23 * b03,
        a12 * b08 - a10 * b11 - a13 * b07, a00 * b11 - a02 * b08 + a03 * b07, a32 * b02 - a30 * b05 - a33 * b01,
        a20 * b05 - a22 * b02 + a23 * b01,
        a10 * b10 - a11 * b08 + a13 * b06, a01 * b08 - a00 * b10 - a03 * b06, a30 * b04 - a31 * b02 + a33 * b00,
        a21 * b02 - a20 * b04 - a23 * b00,
        a11 * b07 - a10 * b09 - a12 * b06, a00 * b09 - a01 * b07 + a02 * b06, a31 * b01 - a30 * b03 - a32 * b00,
        a20 * b03 - a21 * b01 + a22 * b00);
}

// P1.x: camera move in meters that counts as a cut.
[RootSignature(ROOT_SIGNATURE)]
[numthreads(1, 1, 1)]
void CSSetup()
{
    const bool first = (P2.y & FLAG_FIRST) != 0;
    float4 old[10];
    [unroll] for (uint i = 0; i < 9; ++i)
        old[i] = CamOut[i];
    old[9] = CamOut[CAM_INFO];

    const float4x4 inv = float4x4(G[77], G[78], G[79], G[80]);
    const float4x4 vp = inverse4(inv);
    float e = 1.0;
    if ((P2.y & FLAG_EXPOSURE) != 0)
        e = Exposure[0] * G[62].x;
    e = (e > 1e-20 && e < 1e30) ? e : 1.0;
    const float3 position = G[2].xyz;
    // Reversed depth: the larger device depth is the nearer point.
    const bool reversed = length(position_of(inv, 0.5, 0.9)) < length(position_of(inv, 0.5, 0.1));

    const bool cut = first || distance(position, old[8].xyz) > P1.x || !(old[8].w > 0.0);
    if (cut)
    {
        [unroll] for (uint r = 0; r < 4; ++r)
        {
            old[r] = inv[r];
            old[4 + r] = vp[r];
        }
        old[8] = float4(position, e);
    }
    [unroll] for (uint j = 0; j < 9; ++j)
        CamOut[CAM_PREV_INV_VP + j] = old[j];
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        CamOut[CAM_INV_VP + k] = inv[k];
        CamOut[CAM_VP + k] = vp[k];
    }
    CamOut[CAM_POS] = float4(position, e);
    CamOut[CAM_INFO] = float4(reversed ? 1.0 : 0.0, cut ? 1.0 : 0.0, cut ? 0.0 : min(old[9].z + 1.0, 1e6),
                              e / max(old[8].w, 1e-30));
    CamOut[CAM_JITTER] = 0.0;
}

// ---------------------------------------------------------------- prepare: full resolution, P0.xy size

Texture2D<float> GameDepth : register(t0);
Texture2D<float4> GameNormal : register(t1);   // s1 t7: xyz * 2 - 1
Texture2D<float4> GameMaterial : register(t2); // s1 t8: y gloss, x metalness
Texture2D<float4> GameAlbedo : register(t3);   // s1 t6: w material id / 255
Texture2D<float4> GameMotion : register(t4);   // the game's packed motion vectors (see decode_motion)
StructuredBuffer<float4> Cam : register(t7);

RWTexture2D<float> LinearDepthOut : register(u0); // distance from the camera in meters, 1e6 for sky
RWTexture2D<float4> NormalsOut : register(u1);    // world normal, w roughness (1 - gloss), -1 for sky
RWTexture2D<float> PyramidOut : register(u2);     // forward depth, mip 0
RWTexture2D<float2> MotionOut : register(u3);     // uv offset to the previous frame (previous = uv - offset)
RWTexture2D<float4> GlossBefore : register(u4);   // the lighting's export of the previous frame: gloss, drawn
RWTexture2D<float4> GlossNow : register(u5);      // cleared here; this frame's lighting writes it

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void CSPrepare(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.xy))
        return;
    const bool reversed = Cam[CAM_INFO].x > 0.5;
    const float2 uv = (id.xy + 0.5) / float2(P0.xy);
    const float d = GameDepth.Load(int3(id.xy, 0));
    const uint material = (uint)round(GameAlbedo.Load(int3(id.xy, 0)).w * 255.0);
    const bool sky = material == 13;
    const float dist = sky ? 1e6 : min(length(position_of(rows(Cam, CAM_INV_VP), uv, d)), 1e6);
    const float3 n = GameNormal.Load(int3(id.xy, 0)).xyz * 2.0 - 1.0;
    LinearDepthOut[id.xy] = dist;
    PyramidOut[id.xy] = sky ? 1.0 : forward_depth(d, reversed);
    // Motion: the game's where bound, else the camera's (sky as a far point).
    // A camera standing still (photo mode freezes the scene) uses the camera's own motion, zero: the
    // game's vectors there are stale or jittering and made wet armour flicker under our reflection. In play the
    // camera always moves a little and the game's vectors are used.
    const float3 p = ray_of(rows(Cam, CAM_INV_VP), uv) * (sky ? 1e5 : dist);
    const float2 camera_motion = uv - reproject(Cam, p).uv;
    const bool camera_still = length(camera_motion * float2(P0.xy)) < 0.01;
    float2 motion = camera_motion;
    if ((P2.y & FLAG_MOTION) != 0 && !camera_still)
        motion = decode_motion(GameMotion.Load(int3(id.xy, 0)).xyz);
    MotionOut[id.xy] = motion;

    // Roughness and the reflects mask, exactly as the game's lighting used them: each patched variant stores the
    // gloss its probe reads (after every class, snow and puddle path) and a 1 at the pixel (FLAG_GLOSS). Read from
    // the previous frame's export where this pixel was. A pixel no patched variant drew (no probe reflection there:
    // sky, foliage translucency classes, forward shaders, variants without ambient specular) stays 0: not ours.
    // Without the export: t8.y and every lit pixel.
    float gloss = GameMaterial.Load(int3(id.xy, 0)).y;
    bool reflects = !sky;
    if ((P2.y & FLAG_GLOSS) != 0)
    {
        const float2 before = uv - motion;
        const bool inside = all(before >= 0.0) && all(before < 1.0);
        const float2 e = inside ? GlossBefore[min(uint2(before * float2(P0.xy)), P0.xy - 1)].xy : 0.0;
        gloss = e.x;
        reflects = reflects && e.y > 0.5;
    }
    NormalsOut[id.xy] = float4(normalize(n + 1e-6), reflects ? saturate(1.0 - gloss) : -1.0);
    GlossNow[id.xy] = 0.0; // this frame's lighting writes it (all four channels)
}

// ---------------------------------------------------------------- mips (u0 source mip, u1 target mip)
// P0.xy source size, P0.zw target size.

RWTexture2D<float> FloatSource : register(u0);
RWTexture2D<float> FloatTarget : register(u1);
RWTexture2D<float4> ColourSource : register(u0);
RWTexture2D<float4> ColourTarget : register(u1);

// Linear depth at half resolution: a plain sample (the top-left texel), not a min or an average.
[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void CSDepthHalf(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.zw))
        return;
    FloatTarget[id.xy] = FloatSource[min(id.xy * 2, P0.xy - 1)];
}

// The forward-depth pyramid: each texel keeps the nearest of the texels it covers. An odd source size adds the
// last row or column to the last target texel.
[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void CSPyramidDown(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.zw))
        return;
    const uint2 base = id.xy * 2;
    const uint2 last = P0.xy - 1;
    const uint2 extra = uint2(id.x == P0.z - 1 && (P0.x & 1) ? 3 : 2, id.y == P0.w - 1 && (P0.y & 1) ? 3 : 2);
    float m = 1.0;
    for (uint y = 0; y < extra.y; ++y)
        for (uint x = 0; x < extra.x; ++x)
            m = min(m, FloatSource[min(base + uint2(x, y), last)]);
    FloatTarget[id.xy] = m;
}

// ---------------------------------------------------------------- the lit colour capture

Texture2D<float4> Lit : register(t0);

// P0.xy size. Exposed units, kept inside fp16.
[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void CSCapture(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.xy))
        return;
    float3 c = Lit.Load(int3(id.xy, 0)).rgb;
    // NaN or infinity (exponent all ones) becomes 0. fxc drops a plain isnan().
    const bool bad = any((asuint(c) & 0x7F800000u) == 0x7F800000u);
    c = bad ? 0.0 : clamp(c, 0.0, 65000.0);
    ColourTarget[id.xy] = float4(c, 1.0);
}

// Box mips of the capture (for rough reflections and wide indirect light).
[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void CSCaptureDown(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.zw))
        return;
    const uint2 base = id.xy * 2, last = P0.xy - 1;
    ColourTarget[id.xy] = 0.25 * (ColourSource[min(base, last)] + ColourSource[min(base + uint2(1, 0), last)] +
                                  ColourSource[min(base + uint2(0, 1), last)] + ColourSource[min(base + 1, last)]);
}

// ---------------------------------------------------------------- the temporal accumulation (shared)
// t0 this frame's signal (premultiplied rgb, a weight), t1 history, t2 history length, t3 linear depth now,
// t4 linear depth of the previous frame, t5 the shared motion, t7 camera. u0 new history, u1 new length.
// P0.xy pass size, P0.zw full size, P2.z the linear depth mip that matches the pass size, P1.x minimum blend, P1.y maximum length.

Texture2D<float4> Signal : register(t0);
Texture2D<float4> History : register(t1);
Texture2D<float> HistoryLength : register(t2);
Texture2D<float> DepthNow : register(t3);
Texture2D<float> DepthBefore : register(t4);
Texture2D<float2> Motion : register(t5);
RWTexture2D<float4> HistoryOut : register(u0);
RWTexture2D<float> LengthOut : register(u1);

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void CSTemporal(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.xy))
        return;
    const float4 now = Signal[id.xy];
    const uint mip = P2.z;
    const uint2 full = min(uint2(id.xy * float2(P0.zw) / float2(P0.xy)), P0.zw - 1);
    // Mip 0 with a smaller pass (three quarters): full-resolution depth at the mapped pixel.
    const float dist = DepthNow.Load(int3(mip == 0 ? full : id.xy, mip));
    const float2 uv = (full + 0.5) / float2(P0.zw);
    bool valid = (P2.y & FLAG_FIRST) == 0 && Cam[CAM_INFO].y < 0.5 && dist < 1e5;
    reprojected r = (reprojected)0;
    if (valid)
    {
        // Where the pixel was (the shared motion), and the distance a static point there had (the camera).
        r = reproject(Cam, ray_of(rows(Cam, CAM_INV_VP), uv) * dist);
        r.uv = previous_uv(Motion, full, uv);
        valid = all(r.uv >= 0.0) && all(r.uv <= 1.0);
    }
    if (valid)
    {
        const float before = DepthBefore.SampleLevel(PointClamp, r.uv, mip);
        valid = abs(before - r.distance) < 0.05 * r.distance + 0.05;
    }
    if (!valid)
    {
        HistoryOut[id.xy] = now;
        LengthOut[id.xy] = 1.0;
        return;
    }
    // Clamp the history to what this frame shows around the pixel.
    float4 lo = now, hi = now;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float4 s = Signal[clamp(int2(id.xy) + int2(x, y), 0, int2(P0.xy) - 1)];
            lo = min(lo, s);
            hi = max(hi, s);
        }
    const float4 history = clamp(History.SampleLevel(LinearClamp, r.uv, 0), lo, hi);
    const float length = min(HistoryLength.SampleLevel(PointClamp, r.uv, 0) + 1.0, P1.y);
    HistoryOut[id.xy] = lerp(history, now, max(1.0 / length, P1.x));
    LengthOut[id.xy] = length;
}
