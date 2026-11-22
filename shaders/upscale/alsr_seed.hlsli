// MoonSR temporal upscaler: single-frame reconstruction ("seed") = MoonSR Spatial at the output pixel.
// Owner: upscaling-engineer. Used by alsr_seed_ps.hlsl (RenderSingle) and alsr_seedblend_ps.hlsl, which evaluates it
// ONLY for the pixels that need it (no history, rejected or partly trusted history, overlay ring).
//
//   f  = (p + 0.5) * src / dst - 0.5         sample position in source texels (centres at integers)
//   C  = the 4x4 texels floor(f) - 1 .. floor(f) + 2 (clamped to the frame), taken from the 5x5 texels around the
//        nearest texel (see AlsrSeed)
//   out = MoonSR Spatial(C, f - floor(f)), quantised to 8 bit exactly as the former RGBA8 intermediate
// Before Stage 3 this was a separate output-resolution pass (moonsr_spatial 0.24 ms on the RTX 5060 at 720p -> 1080p)
// evaluated for every pixel every frame, although with tracked history most pixels never use it.
#ifndef MOONSR_ALSR_SEED_HLSLI
#define MOONSR_ALSR_SEED_HLSLI

cbuffer SeedConstants : register(b1)
{
    float4 gSeedSrc;   // src w, h, 1/w, 1/h
    float4 gSeedDst;   // dst w, h, min magnification, 0 (must stay 0: see AlsrSeed)
};

#ifndef ALSR_SEED_SRC_REG
#define ALSR_SEED_SRC_REG t0
#endif
Texture2D<float4> gSeedTex : register(ALSR_SEED_SRC_REG); // captured frame, gamma-encoded

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

#include "upscale/moonsr_spatial.hlsli"

int SeedCentreOf(float f) { return int(floor(f + 0.5 + 1e-3)); }

// Gamma-encoded seed colour of output pixel p, 0..1.
// The 4x4 window is selected from the 5x5 texels around the nearest texel c, as in earlier versions (which also fed
// a filter bank from the 5x5 window); this keeps the output bit-identical to them. Loading the 4x4 texels directly
// reads the same values but compiles to different arithmetic (measured: output hashes change, PSNR within 0.0003 dB).
float3 AlsrSeed(uint2 p)
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
    const float3 q = round(saturate(MoonsrSpatial(C, t, gSeedDst.z, 1.0, 0.015)) * 255.0) / 255.0;
    // Exact no-op (gSeedDst.w is always 0) that stops fxc from folding the 1/255 above into the caller's arithmetic
    // (the sRGB decode in alsr_seedblend). Without it the output is not bit-identical to the 8-bit quantised seed of
    // earlier versions (measured: hashes differ, PSNR within 0.0003 dB).
    return asfloat(asuint(q) | asuint(gSeedDst.w));
}

#endif
