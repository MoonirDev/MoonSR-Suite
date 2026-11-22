// Moon ALSR single-frame reconstruction ("seed"): MoonSR Spatial + the per-game learned filter bank, fused.
// Owner: upscaling-engineer. Used by alsr_seed_ps.hlsl (RenderSingle, debug) and inside alsr_reproject_ps.hlsl, which
// evaluates it ONLY for the pixels that need it (no history, rejected or partly trusted history, overlay ring).
// CPU reference: alsr::SpatialCpu + alsr::ApplyCpuRow (src/upscale/alsr_learn.cpp) - window, phase, GeoBucket, the
// 8-bit quantisation of the generic tap and the filter layout must stay identical.
//
//   f  = (p + 0.5) * src / dst - 0.5         sample position in source texels (centres at integers)
//   S  = the 5x5 source texels around c = CentreOf(f) (nearest texel; clamped to the frame)
//   G  = MoonSR Spatial at f from the 4x4 texels floor(f) - 1 .. floor(f) + 2 (always inside S), quantised to 8 bit
//        exactly as the former RGBA8 intermediate (the learner trains on that quantised value)
//   out = w[0] * G + sum_k w[1 + k] * S_k,  w = table[(phaseY * qx + phaseX) * 72 + GeoBucket(luma of S)]
// The published table already contains the blend with the generic filter (w = alpha * learned + (1 - alpha) *
// (1, 0, ...)); with learning off (gSeedMisc.y == 0) the result is G.
// Before Stage 3 this was two output-resolution passes (moonsr_spatial 0.24 ms + alsr_learned 0.18 ms on the RTX 5060
// at 720p -> 1080p) evaluated for every pixel every frame, although with tracked history most pixels never use it.
#ifndef MOONSR_ALSR_SEED_HLSLI
#define MOONSR_ALSR_SEED_HLSLI

cbuffer SeedConstants : register(b1)
{
    float4 gSeedSrc;   // src w, h, 1/w, 1/h
    float4 gSeedDst;   // dst w, h, min magnification, 0
    uint4 gSeedPhase;  // qx, qy, periodic x, periodic y
    uint4 gSeedMisc;   // debug (1 = alpha view), use learned filters, 0, 0
};

#ifndef ALSR_SEED_SRC_REG
#define ALSR_SEED_SRC_REG t0
#define ALSR_SEED_TABLE_REG t1
#endif
Texture2D<float4> gSeedTex : register(ALSR_SEED_SRC_REG); // captured frame, gamma-encoded
Buffer<float4> gSeedTable : register(ALSR_SEED_TABLE_REG); // 7 float4 per bucket: w[0..25], alpha, 0

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

#include "upscale/moonsr_spatial.hlsli"

static const uint kSeedAngles = 8, kSeedStrengths = 3, kSeedCoherences = 3, kSeedGeo = 72, kSeedPhaseBins = 4;

int SeedCentreOf(float f) { return int(floor(f + 0.5 + 1e-3)); }

uint SeedPhaseOf(uint x, float f, uint q, uint periodic)
{
    if (periodic != 0) return x % q;
    const float t = f - float(SeedCentreOf(f)) + 0.5;
    return uint(clamp(int(t * kSeedPhaseBins), 0, int(kSeedPhaseBins) - 1));
}

// Mirror of alsr::GeoBucket (alsr_learn.cpp).
uint SeedGeoBucket(float l[25])
{
    static const float bw[3] = {0.25, 0.5, 0.25};
    float jxx = 0.0, jxy = 0.0, jyy = 0.0;
    [unroll] for (int j = 1; j <= 3; ++j) {
        [unroll] for (int i = 1; i <= 3; ++i) {
            const float gx = 0.5 * (l[j * 5 + i + 1] - l[j * 5 + i - 1]);
            const float gy = 0.5 * (l[(j + 1) * 5 + i] - l[(j - 1) * 5 + i]);
            const float w = bw[i - 1] * bw[j - 1];
            jxx += w * gx * gx;
            jxy += w * gx * gy;
            jyy += w * gy * gy;
        }
    }
    const float tr = jxx + jyy, dxx = jxx - jyy;
    const float disc = sqrt(dxx * dxx + 4.0 * jxy * jxy);
    const float l1 = sqrt(max(0.0, 0.5 * (tr + disc))), l2 = sqrt(max(0.0, 0.5 * (tr - disc)));
    const float coh = (l1 - l2) / (l1 + l2 + 1e-9);
    float ang = 0.5 * atan2(2.0 * jxy, dxx);
    if (ang < 0.0) ang += 3.14159265;
    const uint a = min(uint(ang * (kSeedAngles / 3.14159265)), kSeedAngles - 1);
    const uint s = l1 < 0.015 ? 0u : (l1 < 0.06 ? 1u : 2u);
    const uint c = coh < 0.3 ? 0u : (coh < 0.6 ? 1u : 2u);
    return (a * kSeedStrengths + s) * kSeedCoherences + c;
}

// Gamma-encoded seed colour of output pixel p (not saturated beyond what the filters produce; caller saturates).
// `alpha` = the bucket's learned blend weight (0 = generic) for the debug view.
float3 AlsrSeed(uint2 p, out float alpha)
{
    const float2 f = (float2(p) + 0.5) * gSeedSrc.xy / gSeedDst.xy - 0.5;
    const int2 c = int2(SeedCentreOf(f.x), SeedCentreOf(f.y));
    const float2 base = floor(f);
    const float2 t = f - base;
    const int2 hi = int2(gSeedSrc.xy) - 1;
    float3 S[25];
    [unroll] for (int k = 0; k < 25; ++k) S[k] = gSeedTex.Load(int3(clamp(c + int2(k % 5 - 2, k / 5 - 2), int2(0, 0), hi), 0)).rgb;
    // The 4x4 spatial window starts at base - 1 = c - 2 + (1 - off), off = c - base in {0, 1} per axis.
    const bool ox = c.x != int(base.x), oy = c.y != int(base.y);
    float3 C[16];
    [unroll] for (int j = 0; j < 4; ++j) {
        [unroll] for (int i = 0; i < 4; ++i) {
            const float3 a0 = oy ? S[j * 5 + i] : S[(j + 1) * 5 + i];
            const float3 a1 = oy ? S[j * 5 + i + 1] : S[(j + 1) * 5 + i + 1];
            C[j * 4 + i] = ox ? a0 : a1;
        }
    }
    const float3 g = round(saturate(MoonsrSpatial(C, t, gSeedDst.z, 1.0, 0.015)) * 255.0) / 255.0;
    alpha = 0.0;
    if (gSeedMisc.y == 0) return g;
    float l[25];
    [unroll] for (int k2 = 0; k2 < 25; ++k2) l[k2] = Luma(S[k2]);
    const uint bucket = (SeedPhaseOf(p.y, f.y, gSeedPhase.y, gSeedPhase.w) * gSeedPhase.x + SeedPhaseOf(p.x, f.x, gSeedPhase.x, gSeedPhase.z)) * kSeedGeo +
                        SeedGeoBucket(l);
    const uint b0 = bucket * 7;
    float4 w4 = gSeedTable[b0];
    float3 o = w4.x * g + w4.y * S[0] + w4.z * S[1] + w4.w * S[2];
    [unroll] for (int q = 1; q < 6; ++q) {
        w4 = gSeedTable[b0 + q];
        o += w4.x * S[q * 4 - 1] + w4.y * S[q * 4] + w4.z * S[q * 4 + 1] + w4.w * S[q * 4 + 2];
    }
    w4 = gSeedTable[b0 + 6];
    o += w4.x * S[23] + w4.y * S[24];
    alpha = w4.z;
    return o;
}

#endif
