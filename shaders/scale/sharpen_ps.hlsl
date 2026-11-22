// Contrast-adaptive sharpening, 5-tap cross, run 1:1 over the scaled intermediate
// (srcRectSize == dstSize, srcRect == whole intermediate).
//
// Prior art: the general idea of "contrast-adaptive sharpening" (scaling a sharpen amount by local
// contrast). The formula below is an original, simple design; no vendor code is used.
//
// Let c = centre, n/s/e/w = the four neighbours (RGB, gamma-encoded), avg = (n+s+e+w)/4.
//   L*      = Luma() of each tap.
//   C       = max(L*) - min(L*)                      local luma contrast over the 5 taps
//   gHigh   = 1 - smoothstep(0.35, 0.90, C)          back off where contrast is already high (halos, clipping)
//   gNoise  = smoothstep(0.012, 0.070, C)            fade to zero on near-flat regions (noise, banding)
//   k       = sharpness * KMAX * gHigh * gNoise      luma-derived scalar weight, same for R, G and B
//   out     = c + k * (c - avg)                      unsharp mask on the 4-neighbour mean
//   out     = clamp(out, min(5 taps), max(5 taps))   per channel; forbids over/undershoot
// sharpness == 0 gives k == 0 exactly, so out == c bit-exactly (the clamp range always contains c).
// KMAX is 1.25: with sharpness 1 on a mid-contrast edge this is firm but, because of the clamp and the
// contrast taper, does not produce hard halos. Using one scalar for all channels avoids hue shifts.
//
// Cost: 5 point loads. Edges are replicated by LoadSrc.
#include "common/scale_common.hlsli"

static const float KMAX = 1.25;

float4 main(VsOut i) : SV_Target
{
    int2 p = int2(floor(SrcPosTexels(i.uv)));

    float3 c = LoadSrc(p);
    float3 n = LoadSrc(p + int2(0, -1));
    float3 s = LoadSrc(p + int2(0, 1));
    float3 e = LoadSrc(p + int2(1, 0));
    float3 w = LoadSrc(p + int2(-1, 0));

    float lc = Luma(c), ln = Luma(n), ls = Luma(s), le = Luma(e), lw = Luma(w);
    float lmin = min(lc, min(min(ln, ls), min(le, lw)));
    float lmax = max(lc, max(max(ln, ls), max(le, lw)));
    float contrast = lmax - lmin;

    float gHigh = 1.0 - smoothstep(0.35, 0.90, contrast);
    float gNoise = smoothstep(0.012, 0.070, contrast);
    float k = saturate(sharpness) * KMAX * gHigh * gNoise;

    float3 avg = (n + s + e + w) * 0.25;
    float3 o = c + k * (c - avg);

    float3 mn = min(c, min(min(n, s), min(e, w)));
    float3 mx = max(c, max(max(n, s), max(e, w)));
    o = clamp(o, mn, mx);

    return float4(o, 1.0);
}
