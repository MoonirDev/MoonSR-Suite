// Anti-aliasing, spatial tier: edge-directed luma blend. Owner: shader-engineer.
// Clean-room implementation from the published description of the technique (documentation/research/anti_aliasing.md,
// Part 2): detect a luminance step, orient it from the local gradient, average the two sides of the edge, and blend by
// a sub-pixel weight. No third-party shader code was read or copied.
//
// Gates, each of which exists to protect a kind of content a spatial filter must not touch:
//   1. local step      - the 3x3 luma range must exceed the edge threshold (flat areas and smooth gradients are
//                        untouched);
//   2. axis alignment  - an edge within ~4 degrees of vertical or horizontal is already exact in the pixel grid, so
//                        blending it would only soften it. This is also what keeps vertical and horizontal text stems
//                        and straight UI rules crisp;
//   3. continuation    - the edge must continue along its own direction (taps at 2, 4, 6 and 8 texels must stay on the
//                        centre's side). Isolated dots, stroke tips and noise fail here.
//
// Two cases pass those gates: a real step (two different sides, centre between them) and a thin ridge (one-texel
// highlight, railing, bezel highlight, or a glyph stem - the two sides are the same background). A thin ridge is
// exactly what a railing or a specular edge looks like and it is aliased too, so it is blended, at half weight when
// text protection is on. That is the trade-off the setting exposes: on = gentle on stroke-shaped features, off = the
// same weight as any other edge.
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

    // Gradient across the edge, Sobel-like. Normalised to unit length so the side samples sit exactly `span` texels
    // away along the edge normal at any angle. (Normalising by |gx|+|gy| shortened the offset to 0.71 texels at 45
    // degrees, which pulled both side samples towards the centre pixel and made the blend nearly invisible.)
    const float gx = (lNE + 2.0 * lE + lSE) - (lNW + 2.0 * lW + lSW);
    const float gy = (lSW + 2.0 * lS + lSE) - (lNW + 2.0 * lN + lNE);
    const float g1 = abs(gx) + abs(gy) + 1e-4;
    const float2 across = float2(gx, gy) / max(length(float2(gx, gy)), 1e-4);

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

    // Along-edge taps: they must mostly stay on the centre's side of the edge. This rejects isolated dots, stroke tips
    // and noise, where the "edge" has no continuation in either direction. Sampling at 2, 4, 6 and 8 texels along the
    // edge and counting how many stay close to the centre's luma is a cheap version of that test.
    const float2 tangent = float2(-across.y, across.x);
    const float tol = 0.35 * range;
    float run = 0.0;
    [unroll] for (int k = 1; k <= 4; ++k)
    {
        const float d = 2.0 * k;
        run += abs(AaLuma(AaSample(pos + tangent * d)) - lM) <= tol ? 1.0 : 0.0;
        run += abs(AaLuma(AaSample(pos - tangent * d)) - lM) <= tol ? 1.0 : 0.0;
    }
    const bool continues = run >= 2.0;   // at least one sample each side

    // A real step has two different sides and the centre lies between them. A thin ridge (a one-texel highlight, a
    // railing, a glyph stem) has the background on both sides, so its side contrast is near zero and the centre is
    // outside that interval. It may still be blended - a railing is aliased too - but at a reduced weight when text
    // protection is on, because a stem is exactly the same shape at this scale.
    const bool between = lM >= min(lPos, lNeg) - 0.05 * range && lM <= max(lPos, lNeg) + 0.05 * range;
    const bool thinRidge = sideContrast < threshold;
    const bool ok = (thinRidge || between) && continues;

    float w = 0.0;
    if (ok)
    {
        // How far the centre sits from the two-side average. A pixel that is already the average needs no change (the
        // filter must not compound on content it already smoothed); a staircase step, which sits on one side, gets the
        // full weight. The floor keeps a thin ridge (side contrast near zero) from amplifying to a full blend by
        // accident of division.
        const float sideAvg = (lPos + lNeg) * 0.5;
        const float dev = saturate(abs(lM - sideAvg) / max(sideContrast, 0.25 * range));
        w = aaStrength * blend * saturate(dev * 2.0) * axisGate;
        if (thinRidge && aaProtectText != 0) w *= 0.5;   // half weight on stroke-shaped features
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
