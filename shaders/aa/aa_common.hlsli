// Shared contract for the anti-aliasing shaders. Owner: shader-engineer.
// The C++ mirror of AaConstants is `AaConstants` in src/aa/aa_pass.cpp. Do not change one without the other.
//
// Anti-aliasing (documentation/research/anti_aliasing.md): the captured picture has no depth, motion vectors or MSAA
// samples, so coverage is estimated from luminance transitions and blended toward a directional average. All work is
// done on the gamma-encoded values the capture delivers (the rest of the shader set does the same).
#ifndef MOONSR_AA_COMMON_HLSLI
#define MOONSR_AA_COMMON_HLSLI

cbuffer AaConstants : register(b0)
{
    float2 aaTexSize;          // full source texture size in texels
    float2 aaTexelSize;        // 1 / aaTexSize
    float2 aaContentOrigin;    // top-left of the game image inside the texture, texels
    float2 aaContentSize;      // size of the game image, texels
    float  aaStrength;         // effective strength 0..1 (already reduced by the already-AA detector)
    float  aaEdgeThreshold;    // relative luma contrast that counts as an edge
    float  aaEdgeThresholdMin; // absolute floor for dark scenes
    float  aaOutputScale;      // captured pixels -> output pixels (reserved for later tiers)
    uint   aaQuality;          // AaQuality: 0 Performance, 1 Balanced, 2 Quality
    uint   aaProtectText;      // 1: thin-stroke / text protection on
    uint   aaDebug;            // AaDebugView: 0 None, 1 EdgeMask, 2 BlendWeight, 3 Split
    float  aaPad;
};

Texture2D<float4> srcTex : register(t0);
SamplerState linearClamp : register(s0);

struct AaVsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Perceptual luma of gamma-encoded input (same weights as common/scale_common.hlsli).
float AaLuma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

int2 AaContentLo()
{
    return int2(aaContentOrigin);
}

int2 AaContentHi()
{
    return int2(aaContentOrigin + aaContentSize) - 1;
}

bool AaInsideContent(int2 p)
{
    return all(p >= AaContentLo()) && all(p <= AaContentHi());
}

// Point load, clamped to the game image so the letterbox never bleeds in.
float3 AaLoad(int2 p)
{
    return srcTex.Load(int3(clamp(p, AaContentLo(), AaContentHi()), 0)).rgb;
}

// Bilinear sample at a texel-space position, clamped to the game image.
float3 AaSample(float2 pTexels)
{
    const float2 lo = aaContentOrigin + 0.5;
    const float2 hi = aaContentOrigin + aaContentSize - 0.5;
    return srcTex.SampleLevel(linearClamp, clamp(pTexels, lo, hi) * aaTexelSize, 0).rgb;
}

// Quality presets: distance of the two side samples from the centre (source px) and the blend weight at a full
// staircase step. Quality samples slightly further out and blends harder, which smooths diagonals more.
void AaQualityParams(out float span, out float blend)
{
    if (aaQuality == 0)      { span = 0.95; blend = 0.45; } // Performance
    else if (aaQuality == 2) { span = 1.25; blend = 0.70; } // Quality
    else                     { span = 1.15; blend = 0.55; } // Balanced
}

#endif
