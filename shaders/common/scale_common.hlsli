// Shared contract for all MoonSR scaling / filter pixel shaders. Owned by the lead.
// The C++ mirror of ScaleConstants is `ScaleConstants` in src/upscale/scale_pass.cpp. Do not change one without the other.
#ifndef MOONSR_SCALE_COMMON_HLSLI
#define MOONSR_SCALE_COMMON_HLSLI

cbuffer ScaleConstants : register(b0)
{
    float2 srcTexSize;     // full source texture size in texels
    float2 srcTexelSize;   // 1 / srcTexSize
    float2 srcRectOrigin;  // top-left of the source region in texels
    float2 srcRectSize;    // size of the source region in texels
    float2 dstSize;        // size of the destination rectangle in pixels (== viewport size)
    float2 dstInvSize;     // 1 / dstSize
    float  sharpness;      // 0..1
    float  param1;         // shader specific
    float  param2;         // shader specific
    float  param3;         // shader specific
};

Texture2D<float4> srcTex : register(t0);
SamplerState linearClamp : register(s0);
SamplerState pointClamp  : register(s1);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0; // 0..1 across the destination rectangle
};

// Continuous source position in texels for a destination uv. Texel centres are at integer + 0.5.
float2 SrcPosTexels(float2 uv)
{
    return srcRectOrigin + uv * srcRectSize;
}

// Clamp a texel-space position so that bilinear taps never read outside the source region.
float2 ClampToSrcRect(float2 posTexels)
{
    return clamp(posTexels, srcRectOrigin + 0.5, srcRectOrigin + srcRectSize - 0.5);
}

// Load one texel by integer coordinate, clamped to the source region.
float3 LoadSrc(int2 texel)
{
    int2 lo = int2(srcRectOrigin);
    int2 hi = int2(srcRectOrigin + srcRectSize) - 1;
    return srcTex.Load(int3(clamp(texel, lo, hi), 0)).rgb;
}

// Bilinear sample at a texel-space position, clamped to the source region.
float3 SampleSrcLinear(float2 posTexels)
{
    return srcTex.SampleLevel(linearClamp, ClampToSrcRect(posTexels) * srcTexelSize, 0).rgb;
}

// Perceptual luma approximation for gamma-encoded input.
float Luma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

#endif
