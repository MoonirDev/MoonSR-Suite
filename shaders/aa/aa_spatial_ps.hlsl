// Anti-aliasing, spatial tier: staircase detection and edge reconstruction. Owner: shader-engineer.
// Clean-room implementation from the published description of the technique (documentation/research/anti_aliasing.md,
// Part 2). No third-party shader code was read or copied.
//
// Why this shape: the capture has no coverage information, so what a spatial filter can do is recognise the pixels where
// an edge *turns* inside the pixel (a staircase step) and give them the value a proper area resolve would have produced:
// the average of the two sides. Two properties matter for the result:
//
//   1. the two sides must be sampled *across* the edge, along the axis the edge steps on (N/S for a mostly horizontal
//      edge, E/W for a mostly vertical one). Sampling along the gradient normal instead lands on the diagonal neighbour
//      at 45 degrees, which sits on the same side of a staircase, so the sides look equal and nothing is blended;
//   2. a pixel on the straight part of an edge must be left alone. The test is the luma variation *along* the edge: if
//      the neighbours along it have the same value, the edge runs exactly along the pixel grid and is already resolved.
//      Only where it steps (a corner of the staircase) does the along-edge variation appear, and only those pixels blend.
//
// That also gives text its protection for free: a glyph stem or a straight UI rule is axis-aligned and stays crisp; only
// the few corner pixels of a glyph have a staircase. A thin one-texel ridge (railing, bezel or specular highlight) is
// blended at half weight while "Protect text and thin lines" is on, since at this scale it has the same shape as a stem.
//
// Debug views (aaDebug): 1 EdgeMask (white where a pixel is blended), 2 BlendWeight (the weight as grey), 3 Split (left
// half of the game image unfiltered).
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

    const float3 cN = AaLoad(p + int2( 0, -1));
    const float3 cS = AaLoad(p + int2( 0,  1));
    const float3 cW = AaLoad(p + int2(-1,  0));
    const float3 cE = AaLoad(p + int2( 1,  0));

    const float lM = AaLuma(cM);
    const float lN = AaLuma(cN);
    const float lS = AaLuma(cS);
    const float lW = AaLuma(cW);
    const float lE = AaLuma(cE);

    const float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    const float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    const float range = lMax - lMin;
    const float threshold = max(aaEdgeThresholdMin, lMax * aaEdgeThreshold);

    float w = 0.0;
    float3 average = cM;
    if (range >= threshold)
    {
        const float blendWeight = AaBlendWeight();
        // Which axis does the edge step on? The larger variation is the one across the edge.
        const float lumaNS = abs(lN - lM) + abs(lS - lM);
        const float lumaEW = abs(lE - lM) + abs(lW - lM);
        const bool acrossY = lumaNS >= lumaEW;

        // Variation along the edge: zero on a straight, axis-aligned run (nothing to do), non-zero where the edge steps
        // inside this pixel. 2 % of the range is the noise floor of an 8-bit capture.
        const float along = acrossY ? abs(lE - lW) : abs(lN - lS);
        // High-frequency texture guard: on a checkerboard or a fine pattern *both* along-edge neighbours differ from the
        // centre by the full contrast, while a real staircase keeps at least one of them on the centre's side. Without
        // this the filter would flatten a 2-px checker, which at this scale is indistinguishable from a 2-px step.
        const float alongAgree = acrossY ? min(abs(lE - lM), abs(lW - lM)) : min(abs(lN - lM), abs(lS - lM));
        if (along >= max(0.03, 0.12 * range) && alongAgree <= 0.5 * range)
        {
            const float3 cA = acrossY ? cN : cW;
            const float3 cB = acrossY ? cS : cE;
            const float lA = acrossY ? lN : lW;
            const float lB = acrossY ? lS : lE;
            const float sideContrast = abs(lA - lB);

            // A real step has the centre between its two sides; a thin ridge (one-texel highlight, railing, glyph stem)
            // has both across neighbours on the same side of the centre and far from it. The form is independent of the
            // background, so text on a busy background is recognised as strokes.
            const bool ridgeAcross = (lA - lM) * (lB - lM) > 0.0 &&
                                     abs(lA - lM) > 0.5 * range && abs(lB - lM) > 0.5 * range;
            const bool thinRidge = ridgeAcross;
            const bool between = lM >= min(lA, lB) - 0.10 * range && lM <= max(lA, lB) + 0.10 * range;
            if (between || thinRidge)
            {
                // How far the centre sits from the two-side average, relative to the contrast across the edge. Half at a
                // clean staircase step, smaller where the edge is already soft; the floor keeps a thin ridge from
                // amplifying to a full blend by accident of division.
                const float sideAvg = (lA + lB) * 0.5;
                const float dev = saturate(abs(lM - sideAvg) / max(sideContrast, 0.25 * range));
                w = aaStrength * blendWeight * saturate(dev * 2.0);
                // A stroke-shaped feature is skipped entirely while text protection is on. It is the same shape as a glyph
                // stem at this scale, and the alternative (a reduced weight) measurably dulls synthetic 1-px text.
                if (thinRidge && aaProtectText != 0) w = 0.0;
                average = (cA + cB) * 0.5;
            }
        }
    }
    w = saturate(w);

    if (aaDebug == 1) return float4(w > 0.001 ? 1.0 : 0.0, w > 0.001 ? 1.0 : 0.0, w > 0.001 ? 1.0 : 0.0, 1.0);
    if (aaDebug == 2) return float4(w, w, w, 1.0);

    float3 result = lerp(cM, average, w);
    if (aaDebug == 3)
    {
        const float mid = aaContentOrigin.x + aaContentSize.x * 0.5;
        if (i.pos.x < mid) result = cM;   // A/B: left half unfiltered
    }
    return float4(result, 1.0);
}
