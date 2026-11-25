// Shared by every pass of the scene hook: the root signature, the camera block and the reprojection helper. The
// surface reflections and the indirect lighting use them.
//
// Conventions:
// - Device depth is the game's depth buffer value. Its direction (reversed or not) is found in the setup pass.
//   "Forward depth" fd = depth for a normal buffer and 1 - depth for a reversed one: 0 near, 1 far.
// - Positions are relative to the camera (the game's own lighting works this way: CB2[0][77..80] is the inverse
//   view-projection of camera-relative positions, CB2[0][2].xyz the camera position).
// - Colour is in exposed units: the lighting output before fog and the game's TAA. E is the lighting's exposure
//   (space-1 t0 [0] x CB2[0][62].x). Exposed values keep fp16 precision at night.

#define ROOT_SIGNATURE \
    "RootConstants(num32BitConstants=16, b0), CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=8)), DescriptorTable(UAV(u0, numDescriptors=8)), " \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, " \
    "addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP), " \
    "StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, " \
    "addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

cbuffer PassValues : register(b0)
{
    uint4 P0;   // pass sizes, see each pass
    float4 P1;  // pass values
    uint4 P2;   // x frame, y flags, z mip, w steps
    float4 P3;  // pass values
};

// The game's per-view constants of the scene lighting (its CB2, register b0 space 1 there).
cbuffer Game : register(b1)
{
    float4 G[126];
};

SamplerState PointClamp : register(s0);
SamplerState LinearClamp : register(s1);

// The camera block, one float4 per row (scene_inject.cpp makes it 32 rows).
#define CAM_INV_VP 0     // 4 rows: camera-relative clip -> position (CB2[0][77..80])
#define CAM_VP 4         // 4 rows: its inverse
#define CAM_POS 8        // xyz camera position, w exposure E
#define CAM_PREV_INV_VP 9
#define CAM_PREV_VP 13
#define CAM_PREV_POS 17  // xyz, w exposure of the previous frame
#define CAM_INFO 18      // x 1 for reversed depth, y 1 on a camera cut, z frames since the cut, w E / E_prev
#define CAM_JITTER 19    // not found yet: 0
#define CAM_ROWS 32

#define FLAG_FIRST 1u            // first frame or new resources: no history anywhere
#define FLAG_EXPOSURE 2u         // the exposure buffer is bound
#define FLAG_CAPTURE 4u          // the lit colour capture holds a frame
#define FLAG_MOTION 32u          // the game's motion vectors are bound (else camera motion only)
#define FLAG_GLOSS 512u          // the lighting exports its final gloss and reflects mask (see CSPrepare)

// The game's motion vectors (the G-buffer target its AO history pass C9582762947B8EC4 reads at space-4 t4): two 12-bit
// values in the 8-bit x, y, z channels, v in -1..1, uv offset 0.2 sign(v) v^4. The previous uv is uv minus it.
float2 decode_motion(float3 t)
{
    const float high = floor(t.y * 15.9375);
    const float2 n = float2(t.x * 4080.0 + high, frac(t.y * 15.9375) * 4096.0 + t.z * 255.0);
    const float2 v = clamp(n * asfloat(0x3A001002u) - 1.0, -1.0, 1.0);
    return 0.2 * sign(v) * (v * v) * (v * v);
}

float4x4 rows(StructuredBuffer<float4> cam, uint at)
{
    return float4x4(cam[at], cam[at + 1], cam[at + 2], cam[at + 3]);
}

float2 to_ndc(float2 uv)
{
    return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

float2 to_uv(float2 ndc)
{
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float forward_depth(float device_depth, bool reversed)
{
    return reversed ? 1.0 - device_depth : device_depth;
}

float device_depth(float fd, bool reversed)
{
    return reversed ? 1.0 - fd : fd;
}

// Camera-relative position of a pixel.
float3 position_of(float4x4 inv_vp, float2 uv, float device)
{
    const float4 p = mul(inv_vp, float4(to_ndc(uv), device, 1.0));
    return p.xyz / p.w;
}

// Direction from the camera through a pixel (unit length).
float3 ray_of(float4x4 inv_vp, float2 uv)
{
    return normalize(position_of(inv_vp, uv, 0.5));
}

// Camera-relative position -> (uv, forward depth, w).
float4 project(float4x4 vp, float3 p, bool reversed)
{
    const float4 c = mul(vp, float4(p, 1.0));
    const float3 n = c.xyz / c.w;
    return float4(to_uv(n.xy), forward_depth(n.z, reversed), c.w);
}

// The one reprojection helper: where a surface point seen this frame was in the previous frame, and the distance
// it then had from the camera (to compare with the previous frame's linear depth), from the camera alone. Passes
// that know the pixel use the shared motion texture instead (scene.hlsl CSPrepare: the game's motion vectors, or
// this camera motion where they are not bound), see previous_uv.
struct reprojected
{
    float2 uv;
    float distance;
    bool inside;
};

reprojected reproject(StructuredBuffer<float4> cam, float3 position)
{
    reprojected r;
    const float3 world = position + cam[CAM_POS].xyz;
    const float3 prev = world - cam[CAM_PREV_POS].xyz;
    const float4 c = mul(rows(cam, CAM_PREV_VP), float4(prev, 1.0));
    r.uv = to_uv(c.xy / c.w);
    r.distance = length(prev);
    r.inside = c.w > 0.0 && all(r.uv >= 0.0) && all(r.uv <= 1.0);
    return r;
}

// Where a pixel was in the previous frame: its uv minus the shared motion texture (uv units).
float2 previous_uv(Texture2D<float2> motion, uint2 pixel, float2 uv)
{
    return uv - motion.Load(int3(pixel, 0));
}

float luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// PCG hash, for per-pixel per-frame random numbers (no blue noise texture yet).
uint pcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float2 random2(uint2 pixel, uint frame)
{
    const uint a = pcg(pixel.x + pcg(pixel.y + pcg(frame)));
    const uint b = pcg(a);
    return float2(a & 0xFFFFFF, b & 0xFFFFFF) / 16777216.0;
}
