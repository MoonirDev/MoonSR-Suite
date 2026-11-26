// Anti-aliasing, spatial tier (T1): edge-directed luma blend. Owner: shader-engineer.
// Clean-room implementation from the published description of the technique (documentation/research/anti_aliasing.md,
// Part 2): detect a luminance step, orient it from the local gradient, average the two sides of the edge, and blend by
// a sub-pixel weight. No third-party shader code was read or copied.
//
// Three gates decide whether a pixel may be blended. Each one exists to protect a kind of content that a spatial
// filter must not touch:
//   1. local step      - the 3x3 luma range must exceed the edge threshold (flat areas are untouched);
//   2. axis alignment  - an edge within ~5 degrees of vertical or horizontal is already exact in the pixel grid, so
//                        blending it would only soften it. This also protects vertical and horizontal text stems;
//   3. two-sidedness   - the samples one texel to either side of the edge must differ strongly (a real edge has two
//                        different sides) and the centre must lie between them (a thin line or a small detail is
//                        outside). A checker texture has the same colour on both sides; a 1 px stroke has the
//                        background on both sides. Both are rejected here.
//
// Debug views (aaDebug): 1 EdgeMask (white where a pixel would be blended), 2 BlendWeight (the weight as grey),
// 3 Split (left half of the game image unfiltered).
#include "aa/aa_common.hlsli"

float4 main(AaVsOut i) : SV_Target
{
    const int2 p = int2(i.pos.xy);
    const float3 cM = AaLoad(p);
    if (!AaInsideContent(p)) return float4(cM, 1.0);   // letterbox: untouched, bit for bit
    if (aaStrength <= 0.0)
    {
        if (aaDebug == 1 || aaDebug == 2) return float4(0.0, 0.0, 0.0, 1.0);
        return float4(cM, 1.0);
    }

    const float lM = AaLuma(cM);
    const float lN = AaLuma(AaLoad(p + int2( 0, -1)));
    const float lS = AaLuma(AaLoad(p + int2( 0,  1)));
    const float lW = AaLuma(AaLoad(p + int2(-1,  0)));
    const float lE = AaLuma(AaLoad(p + int2( 1,  0)));
    const float lNW = AaLuma(AaLoad(p + int2(-1, -1)));
    const float lNE = AaLuma(AaLoad(p + int2( 1, -1)));
    const float lSW = AaLuma(AaLoad(p + int2(-1,  1)));
    const float lSE = AaLuma(AaLoad(p + int2( 1,  1)));

    const float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    const float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    const float range = lMax - lMin;
    const float threshold = max(aaEdgeThresholdMin, lMax * aaEdgeThreshold);

    // Gate 1: a flat neighbourhood has nothing to anti-alias.
    if (range < threshold)
    {
        if (aaDebug == 1 || aaDebug == 2) return float4(0.0, 0.0, 0.0, 1.0);
        return float4(cM, 1.0);
    }

    // Gradient across the edge, Sobel-like. L1-normalised so the offset stays within one texel in total.
    const float gx = (lNE + 2.0 * lE + lSE) - (lNW + 2.0 * lW + lSW);
    const float gy = (lSW + 2.0 * lS + lSE) - (lNW + 2.0 * lN + lNE);
    const float g1 = abs(gx) + abs(gy) + 1e-4;
    const float2 across = float2(gx, gy) / g1;

    // Gate 2: 1.0 for an edge that is exactly vertical or horizontal (already resolved by the pixel grid),
    // 0.5 for a 45 degree edge. Fade the blend out over the last ~5 degrees on either side.
    const float axisAlign = max(abs(gx), abs(gy)) / g1;
    const float axisGate = 1.0 - saturate((axisAlign - 0.92) / 0.08);

    float span, blend;
    AaQualityParams(span, blend);

    // The two sides of the edge, one texel away along the across vector. `span` is in texels: Performance samples
    // slightly closer (sharper), Quality slightly further (smoother).
    const float2 pos = i.pos.xy;
    const float3 cPos = AaSample(pos + across * span);
    const float3 cNeg = AaSample(pos - across * span);
    const float lPos = AaLuma(cPos);
    const float lNeg = AaLuma(cNeg);
    const float sideContrast = abs(lPos - lNeg);

    // Along-edge neighbours: for a real edge they sit on the same side as the centre, so their luma is close to it.
    // At a corner (the end of a text stroke, the tip of a small detail) or inside a checker texture, one of them
    // lands on a different feature and the blend is cancelled. This is the text / high-frequency protection.
    const float2 tangent = float2(-across.y, across.x);
    const float lT1 = AaLuma(AaSample(pos + tangent * span));
    const float lT2 = AaLuma(AaSample(pos - tangent * span));
    const bool alongOk = abs(lT1 - lM) <= 0.5 * range && abs(lT2 - lM) <= 0.5 * range;

    // Gate 3a: both sides must be clearly different from each other. A checker or a one-pixel line has the same
    // colour on both sides, so its side contrast is near zero.
    const bool twoSided = sideContrast >= threshold;

    // Gate 3b: the centre must lie between the two sides (inclusive, with a small tolerance for noise). A thin line
    // or a small detail is outside that interval.
    const float sideLo = min(lPos, lNeg) - 0.05 * range;
    const float sideHi = max(lPos, lNeg) + 0.05 * range;
    const bool between = lM >= sideLo && lM <= sideHi;

    float w = 0.0;
    // The stroke guards (3b and the along-edge test) are the text / HUD protection; turning it off keeps the
    // two-sidedness test, which is what stops the filter from turning a one-pixel line into a smear.
    const bool guarded = aaProtectText != 0 ? (between && alongOk) : true;
    if (twoSided && guarded)
    {
        // How far the centre sits from the two-side average, relative to the side contrast. A pixel that is already
        // the average needs no change (the filter must not compound on content it already smoothed); a pure
        // staircase step, which sits on one side, gets the full weight. dev is 0.5 at a hard step, 0 at the average.
        const float sideAvg = (lPos + lNeg) * 0.5;
        const float dev = saturate(abs(lM - sideAvg) / max(sideContrast, 1e-4));
        w = aaStrength * blend * saturate(dev * 2.0) * axisGate;
    }
    w = saturate(w);

    if (aaDebug == 1) return float4(w > 0.001 ? 1.0 : 0.0, w > 0.001 ? 1.0 : 0.0, w > 0.001 ? 1.0 : 0.0, 1.0);
    if (aaDebug == 2) return float4(w, w, w, 1.0);

    const float3 average = (cPos + cNeg) * 0.5;
    float3 result = lerp(cM, average, w);
    if (aaDebug == 3)
    {
        const float mid = aaContentOrigin.x + aaContentSize.x * 0.5;
        if (i.pos.x < mid) result = cM;   // A/B: left half unfiltered
    }
    return float4(result, 1.0);
}
