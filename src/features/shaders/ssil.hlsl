// Screen-space indirect light (one diffuse bounce) from a visibility bitmask.
//
// The method is the visibility bitmask of the paper "Screen Space Indirect Lighting with Visibility Bitmask",
// written for this add-on with the sector mapping of the GTAO cosine-weighted measure: each
// slice's hemisphere around the normal is split into 32 sectors of equal share of the cosine-weighted measure, so a
// fully open pixel gives V = 1 and an occluder T meters thick marks the sectors between its front and its back.
// A sample that marks sectors still open is what the pixel sees through them: its radiance (last frame's lit colour
// there) is added with the share of the newly marked sectors. Sectors already marked add nothing, so a near occluder
// hides a farther one.
//
// Per pixel: V = sum_s w_s (1 - popcount(mask_s) / 32) / sum_s w_s and S = sum_s w_s sum_new (popcount(new) / 32) L
// / sum_s w_s, with w_s = |projected normal| (cos n + n sin n), the open slice value of the game's GTAO. S is
// irradiance in radiance units (no 1/pi), like the ambient the lighting multiplies by albedo, in this frame's exposed
// units.

#include "common.hlsli"

#define PI 3.14159265

#define FLAG_HALF 524288u // the passes before the final one run at half resolution



// t0 linear depth (mip 0 full, 1 half), t1 normals (full), t2 capture (last frame, exposed, mips), t3 the shared
// motion (full), t7 camera. u0: rgb S, a V.
// P0.xy trace size, P0.zw full size. P1: x radius (m), y thickness (m), z falloff start (share of the radius),
// w step spread. P2: x frame, y flags, z depth mip, w slices | steps << 16. P3: x luminance cap (exposed units),
// y back-face band, z max radius in full-resolution pixels.

Texture2D<float> LinearDepth : register(t0);
Texture2D<float4> Normals : register(t1);
Texture2D<float4> Capture : register(t2);
Texture2D<float2> Motion : register(t3);
StructuredBuffer<float4> Cam : register(t7);
RWTexture2D<float4> Out : register(u0);

// The share of the slice's cosine-weighted measure from the hemisphere's edge n - pi/2 up to angle a (from V, signed:
// + toward the slice direction), times W = cos n + n sin n.
float measure(float a, float n, float cn, float sn)
{
    const float sg = a < 0.0 ? -1.0 : 1.0;
    return sg * (0.25 * cn - 0.25 * cos(2.0 * a - n)) + 0.5 * abs(a) * sn;
}

// Sectors [floor(32 lo + j), floor(32 hi + j)) as bits (v2 centred rounding with a dither j).
uint sectors(float lo, float hi, float j)
{
    const int s = clamp((int)floor(32.0 * lo + j), 0, 32), e = clamp((int)floor(32.0 * hi + j), 0, 32);
    const int c = e - s;
    const uint run = c >= 32 ? 0xFFFFFFFFu : (((1u << (uint)clamp(c, 0, 31)) - 1u) << (uint)min(s, 31));
    return c <= 0 ? 0u : run;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void SSILTrace(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.xy))
        return;
    const float2 size = float2(P0.zw), tsize = float2(P0.xy);
    const float scale = size.x / tsize.x;
    const uint mip = P2.z;
    const uint2 full = min(uint2(id.xy * size / tsize), P0.zw - 1);
    const float dist = LinearDepth.Load(int3(id.xy, mip));
    if (dist >= 1e5 || (P2.y & FLAG_CAPTURE) == 0)
    {
        Out[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const float4x4 inv = rows(Cam, CAM_INV_VP), vp = rows(Cam, CAM_VP);
    const bool reversed = Cam[CAM_INFO].x > 0.5;
    const float2 uv = (full + 0.5) / size;
    const float3 pos = ray_of(inv, uv) * dist;
    const float3 v = -pos / dist;
    const float3 n = Normals.Load(int3(full, 0)).xyz;
    const float radius = P1.x, thick = P1.y;
    const float fstart = P1.z * radius, frange = max((1.0 - P1.z) * radius, 1e-4);
    const uint slices = max(P2.w & 0xFFFFu, 1u), steps = max(P2.w >> 16, 1u);
    const float2 noise = random2(id.xy, P2.x);
    const float exposure_ratio = Cam[CAM_INFO].w;

    float vis = 0.0, wsum = 0.0, known = 0.0, unknown = 0.0;
    float3 light = 0.0;
    for (uint s = 0; s < slices; ++s)
    {
        const float phi = (s + noise.x) * PI / slices;
        const float2 dir = float2(cos(phi), sin(phi)); // in pixels, y down
        // The slice plane holds the view ray and the screen line through the pixel along dir. d3: its direction
        // across the view ray, toward +dir.
        const float3 q = ray_of(inv, uv + dir / size) * dist;
        float3 d3 = q - pos;
        d3 = normalize(d3 - v * dot(d3, v) + 1e-12);
        const float3 axis = cross(d3, v);
        const float3 pn = n - axis * dot(n, axis);
        const float pl = length(pn);
        if (pl < 1e-4)
            continue;
        const float na = clamp(atan2(dot(pn, d3), dot(pn, v)), -PI * 0.5 + 1e-3, PI * 0.5 - 1e-3);
        const float cn = cos(na), sn = sin(na);
        const float open = cn + na * sn;
        const float w = pl * open;
        const float m0 = measure(na - PI * 0.5, na, cn, sn);
        const float inv_open = 1.0 / open;
        // The radius on screen along this slice, in full-resolution pixels.
        const float4 e = project(vp, pos + d3 * radius, reversed);
        float rpx = e.w > 1e-4 ? length((e.xy - uv) * size) : P3.z;
        rpx = min(rpx, P3.z);
        const float rt = rpx / scale; // trace pixels
        const float smip = clamp(log2(max(rpx / steps, 1.0)) - 1.0, 0.0, 4.0);
        // Per slice dither of the sector grid.
        const float j = frac(noise.y + 0.618034 * s + 0.75487767 * (P2.x & 255));
        uint mask = 0u;
        if (rt >= 1.0)
        {
            for (uint k = 0; k < steps; ++k)
            {
                const float t = (k + frac(noise.y * 7.0 + 0.5 * s)) / steps;
                const float off = 1.0 + (rt - 1.0) * pow(t, P1.w);
                [unroll] for (int side = 0; side < 2; ++side)
                {
                    const float sg = side == 0 ? 1.0 : -1.0;
                    const int2 sp = int2(floor(id.xy + 0.5 + sg * dir * off));
                    if (any(sp < 0) || any(sp >= int2(P0.xy)))
                        continue;
                    const float sd = LinearDepth.Load(int3(sp, mip));
                    if (sd >= 1e5)
                        continue;
                    const uint2 sf = min(uint2(sp * size / tsize), P0.zw - 1);
                    const float2 suv = (sf + 0.5) / size;
                    const float3 d = ray_of(inv, suv) * sd - pos;
                    const float len = length(d);
                    if (len < 1e-4 || len > radius)
                        continue;
                    const float e1 = dot(d, v);
                    // The back of the sample: T meters further along the pixel's view ray.
                    const float lb = sqrt(max(len * len - 2.0 * thick * e1 + thick * thick, 1e-8));
                    const float fall = 2.0 * saturate((len - fstart) / frange);
                    const float cf = clamp(e1 / len - fall, -1.0, 1.0), cb = clamp((e1 - thick) / lb - fall, -1.0, 1.0);
                    float a0 = sg * acos(cf), a1 = sg * acos(cb);
                    float lo = clamp(min(a0, a1), na - PI * 0.5, na + PI * 0.5);
                    float hi = clamp(max(a0, a1), na - PI * 0.5, na + PI * 0.5);
                    const float ulo = (measure(lo, na, cn, sn) - m0) * inv_open;
                    const float uhi = (measure(hi, na, cn, sn) - m0) * inv_open;
                    const uint bits = sectors(ulo, uhi, j);
                    const uint fresh = bits & ~mask;
                    if (fresh == 0u)
                        continue;
                    mask |= bits;
                    const float share = w * countbits(fresh) / 32.0;
                    // Back faces toward the pixel send nothing.
                    const float3 ns = Normals.Load(int3(sf, 0)).xyz;
                    const float facing = P3.y > 0.0 ? saturate(dot(ns, -d / len) / P3.y) : (dot(ns, -d) > 0.0 ? 1.0 : 0.0);
                    const float2 before = suv - Motion.Load(int3(sf, 0));
                    if (all(before >= 0.0) && all(before <= 1.0))
                    {
                        float3 c = Capture.SampleLevel(LinearClamp, before, smip).rgb * exposure_ratio;
                        const float l = luminance(c);
                        c *= l > P3.x ? P3.x / l : 1.0;
                        light += share * facing * max(c, 0.0);
                        known += share;
                    }
                    else
                        unknown += share; // off screen last frame: filled with the mean of the known ones
                }
            }
        }
        vis += w * (1.0 - countbits(mask) / 32.0);
        wsum += w;
    }
    if (wsum <= 0.0)
    {
        Out[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    if (known > 0.0)
        light += light * (unknown / known);
    Out[id.xy] = float4(light / wsum, vis / wsum);
}

// ---------------------------------------------------------------- spatial (a-trous, 3x3, one pass per step size)
// t0 the temporal result, t1 linear depth, t2 normals. P0.xy pass size, P0.zw full size, P1.x step (pixels),
// P1.y depth tolerance (share of the distance), P1.z normal power. P2.z depth mip.

Texture2D<float4> Accumulated : register(t0);
Texture2D<float> PassDepth : register(t1);
Texture2D<float4> PassNormals : register(t2);
Texture2D<float4> THistory : register(t4);
Texture2D<float> TLength : register(t5);
Texture2D<float> TDepthBefore : register(t6);
RWTexture2D<float> TLengthOut : register(u2);

// The temporal accumulation: scene.hlsl CSTemporal (the same reprojection, disocclusion test, length and blend)
// without its history clamp. A clamp to this frame's neighbours pulled sparse bright history to 0. t0 this frame's
// trace, t1 linear depth now, t3 the shared motion, t4 history, t5 history length, t6 linear depth before. u0 new
// history, u2 new length. P0.xy pass size, P0.zw full size, P1.x minimum blend, P1.y maximum length, P2.z depth mip.
[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void SSILTemporal(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy >= P0.xy))
        return;
    const uint2 id = tid.xy;
    const float4 now = Accumulated[id];
    const uint mip = P2.z;
    const float dist = PassDepth.Load(int3(id, mip));
    const uint2 full = min(uint2(id * float2(P0.zw) / float2(P0.xy)), P0.zw - 1);
    const float2 uv = (full + 0.5) / float2(P0.zw);
    bool valid = (P2.y & FLAG_FIRST) == 0 && Cam[CAM_INFO].y < 0.5 && dist < 1e5;
    reprojected r = (reprojected)0;
    if (valid)
    {
        r = reproject(Cam, ray_of(rows(Cam, CAM_INV_VP), uv) * dist);
        r.uv = previous_uv(Motion, full, uv);
        valid = all(r.uv >= 0.0) && all(r.uv <= 1.0);
    }
    if (valid)
    {
        const float before = TDepthBefore.SampleLevel(PointClamp, r.uv, mip);
        valid = abs(before - r.distance) < 0.05 * r.distance + 0.05;
    }
    if (!valid)
    {
        Out[id] = now;
        TLengthOut[id] = 1.0;
        return;
    }
    const float4 history = THistory.SampleLevel(LinearClamp, r.uv, 0);
    const float length = min(TLength.SampleLevel(PointClamp, r.uv, 0) + 1.0, P1.y);
    Out[id] = lerp(history, now, max(1.0 / length, P1.x));
    TLengthOut[id] = length;
}

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void SSILSpatial(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.xy))
        return;
    const float2 scale = float2(P0.zw) / float2(P0.xy);
    const float d = PassDepth.Load(int3(id.xy, P2.z));
    const float4 centre = Accumulated[id.xy];
    if (d >= 1e5)
    {
        Out[id.xy] = centre;
        return;
    }
    const float3 n = PassNormals.Load(int3(min(uint2(id.xy * scale), P0.zw - 1), 0)).xyz;
    const int step = max((int)P1.x, 1);
    float4 sum = 0.0;
    float weight = 0.0;
    [unroll] for (int j = 0; j < 9; ++j)
    {
        const int x = j % 3 - 1, y = j / 3 - 1;
        const int2 q = clamp(int2(id.xy) + int2(x, y) * step, 0, int2(P0.xy) - 1);
        const float dq = PassDepth.Load(int3(q, P2.z));
        const float3 nq = PassNormals.Load(int3(min(uint2(q * scale), P0.zw - 1), 0)).xyz;
        const float k = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
        const float wq = dq >= 1e5 ? 0.0 : k * exp(-abs(dq - d) / (P1.y * d + 0.01)) * pow(saturate(dot(n, nq)), P1.z);
        sum += Accumulated[q] * wq;
        weight += wq;
    }
    Out[id.xy] = weight > 0.0 ? sum / weight : centre;
}

// ---------------------------------------------------------------- final: the texture the lighting reads (full size)
// t0 the filtered result (pass size), t1 linear depth, t2 normals. u0 the lighting's texture (space-2 t15
// there): rgb = what is added to the ambient, in exposed units (the lighting divides by its exposure), a = 1 + our
// visibility. P0.xy full size, P0.zw pass size. P1.x strength. P2.y flags.

[RootSignature(ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void SSILFinal(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= P0.xy))
        return;
    const float d = PassDepth.Load(int3(id.xy, 0));
    float4 r;
    if ((P2.y & FLAG_HALF) != 0)
    {
        // Joint bilateral 2x2 upsample on depth.
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
                const float w = bilinear * exp(-abs(PassDepth.Load(int3(q, 1)) - d) / (0.02 * d + 0.01)) + 1e-5;
                sum += Accumulated[q] * w;
                weight += w;
            }
        r = sum / weight;
    }
    else
        r = Accumulated[id.xy];
    // Broken values or no receiver (sky): exactly 0, the lighting then keeps the game's ambient bit for bit.
    const bool bad = any((asuint(r) & 0x7F800000u) == 0x7F800000u);
    const bool none = bad || d >= 1e5;
    const float3 s = none ? 0.0 : max(r.rgb, 0.0);
    const float vis = none ? 1.0 : saturate(r.a);
    // a = 1 + V where we have a value. 0 stays "nothing": the lighting keeps the game's ambient there.
    Out[id.xy] = none ? 0.0 : float4(s * P1.x, 1.0 + vis);
}

