// Interpolate stage 5: guided anti-aliasing of the generated frame (generated frames only; real frames never pass
// through here). Drawn with common/fullscreen_vs.hlsl over a viewport of exactly the frame size into the caller's
// target. Original MoonSR filter.
//
// The guide g (fg_interpolate_ps: motion boundaries, disagreement of the two warped samples, partial fallback; zero
// on static content, the HUD and wherever the generated frame is a confident warp) gates everything, so real-frame
// detail carried by a good warp and static HUD text keep their exact pixels.
// Where g > 0:
//  1. Outlier suppression: the centre colour is pulled into the [min, max] box of its 8 neighbours by g. Isolated
//     wrong pixels (a mismatched hypothesis winning one pixel) disappear; edges are untouched (an edge pixel is inside
//     its neighbours' range).
//  2. Edge-directed smoothing: the edge tangent comes from a 3x3 Sobel on luma; two bilinear taps at +-0.75 px and
//     two at +-2 px along the tangent are averaged with the centre (1/4, 1/2, 1/4). The far pair is dropped when its
//     luma leaves the 3x3 range (it crossed another edge). Smoothing runs ALONG the edge only, so stair steps are
//     spread out without blurring across the edge. Strength g * saturate(luma range / 0.08).
//   gSrcSize frame size   gU1.y = guide view (development: shows the guide instead of the colour)
#include "framegen/fg_common.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> gGen : register(t0);
Texture2D<float> gGuide : register(t1);
SamplerState gLinear : register(s0);

float3 At(int2 p) { return gGen.Load(int3(ClampI(p, gSrcSize), 0)).rgb; }

float4 main(VsOut i) : SV_Target
{
    const int2 p = int2(i.pos.xy);
    const float g = gGuide.Load(int3(p, 0));
    float3 c = gGen.Load(int3(p, 0)).rgb;
    if (gU1.y != 0) return float4(g, g, g, 1.0);
    [branch] if (g < 1.0 / 255.0) return float4(c, 1.0);

    const float3 n = At(p + int2(0, -1)), s = At(p + int2(0, 1)), e = At(p + int2(1, 0)), w = At(p + int2(-1, 0));
    const float3 ne = At(p + int2(1, -1)), nw = At(p + int2(-1, -1)), se = At(p + int2(1, 1)), sw = At(p + int2(-1, 1));
    const float3 nMin = min(min(min(n, s), min(e, w)), min(min(ne, nw), min(se, sw)));
    const float3 nMax = max(max(max(n, s), max(e, w)), max(max(ne, nw), max(se, sw)));
    c = lerp(c, clamp(c, nMin, nMax), g);

    const float lC = Luma(c), lN = Luma(n), lS = Luma(s), lE = Luma(e), lW = Luma(w);
    const float lNE = Luma(ne), lNW = Luma(nw), lSE = Luma(se), lSW = Luma(sw);
    const float lMin = min(lC, min(min(min(lN, lS), min(lE, lW)), min(min(lNE, lNW), min(lSE, lSW))));
    const float lMax = max(lC, max(max(max(lN, lS), max(lE, lW)), max(max(lNE, lNW), max(lSE, lSW))));
    const float gx = (lNE + 2.0 * lE + lSE) - (lNW + 2.0 * lW + lSW);
    const float gy = (lSW + 2.0 * lS + lSE) - (lNW + 2.0 * lN + lNE);
    const float gl = sqrt(gx * gx + gy * gy);
    const float range = lMax - lMin;
    [branch] if (gl < 1e-3 || range < 2.0 / 255.0) return float4(c, 1.0);

    const float2 inv = 1.0 / float2(gSrcSize);
    const float2 uv = (float2(p) + 0.5) * inv;
    const float2 d = float2(-gy, gx) / gl * inv;
    const float3 a0 = gGen.SampleLevel(gLinear, uv + 0.75 * d, 0).rgb;
    const float3 a1 = gGen.SampleLevel(gLinear, uv - 0.75 * d, 0).rgb;
    const float3 b0 = gGen.SampleLevel(gLinear, uv + 2.0 * d, 0).rgb;
    const float3 b1 = gGen.SampleLevel(gLinear, uv - 2.0 * d, 0).rgb;
    const float3 nearAvg = 0.5 * (a0 + a1);
    const float3 farAvg = 0.5 * (b0 + b1);
    const float lFar = Luma(farAvg);
    const float3 aa = (lFar < lMin || lFar > lMax) ? 0.5 * (c + nearAvg) : 0.25 * c + 0.5 * nearAvg + 0.25 * farAvg;
    const float strength = g * saturate(range / 0.08);
    return float4(lerp(c, aa, strength), 1.0);
}
