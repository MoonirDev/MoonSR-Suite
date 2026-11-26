// Anti-aliasing, spatial tier: simple FXAA-style filter. Owner: shader-engineer.
//
// This follows the published description of FXAA (Lottes, "FXAA", 2009 / FXAA 3.11 from the NVIDIA GameWorks
// repository, BSD-3-Clause): estimate the local edge from a luma cross, pick the axis the edge steps on, average the
// two sides, and soften along the edge as well so a staircase reads as a ramp. It is written here from the description;
// no third-party shader code was copied.
//
// Compared with the previous morphological build this is deliberately blunt: every detected edge is blended, including
// thin one-texel ridges, which is what makes FXAA visibly smooth - and what softens text. "Protect text and thin lines"
// keeps the ridge case out of the blend; straight axis-aligned runs are still skipped because there is nothing to
// resolve there.
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

    const int2 offN = int2(0, -1), offS = int2(0, 1), offW = int2(-1, 0), offE = int2(1, 0);

    const float3 cN = AaLoad(p + offN);
    const float3 cS = AaLoad(p + offS);
    const float3 cW = AaLoad(p + offW);
    const float3 cE = AaLoad(p + offE);

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
    float3 blended = cM;
    if (range >= threshold)
    {
        // Which axis does the edge step on? The larger variation is the one across the edge.
        const float lumaNS = abs(lN - lM) + abs(lS - lM);
        const float lumaEW = abs(lE - lM) + abs(lW - lM);
        const bool horzSpan = lumaNS >= lumaEW;    // horizontal edge: the step is between N and S

        const float3 cA = horzSpan ? cN : cW;    // one side
        const float3 cB = horzSpan ? cS : cE;    // the other side
        const float3 cL = horzSpan ? cW : cN;    // along the edge
        const float3 cR = horzSpan ? cE : cS;
        const float lA = horzSpan ? lN : lW;
        const float lB = horzSpan ? lS : lE;

        // A one-texel ridge (railing, wire, glyph stem) has the same background on both sides. FXAA blends it; the
        // text protection keeps it crisp. Without the protection it is treated like any other edge.
        const bool ridge = (lA - lM) * (lB - lM) > 0.0 && abs(lA - lM) > 0.5 * range && abs(lB - lM) > 0.5 * range;
        // Variation along the edge: zero on a straight axis-aligned run, which the pixel grid already resolves.
        const float along = horzSpan ? abs(lE - lW) : abs(lN - lS);
        const bool straight = along < max(0.02, 0.10 * range);

        if ((!ridge || aaProtectText == 0) && !straight)
        {
            // FXAA core: the two sides averaged. That is the value an area resolve would have produced for a pixel the
            // edge crosses, and it is what turns a staircase into a ramp. The along-edge neighbours are deliberately not
            // mixed in: at a staircase corner they sit on the far side, so averaging them pulls the colour past the two
            // sides and brings the step back (measured: the strongest preset made the step *larger*).
            const float3 rgbA = (cA + cB) * 0.5;
            blended = rgbA;
            w = aaStrength * AaBlendWeight();
        }
    }
    w = saturate(w);

    if (aaDebug == 1) return float4(w > 0.001 ? 1.0 : 0.0, w > 0.001 ? 1.0 : 0.0, w > 0.001 ? 1.0 : 0.0, 1.0);
    if (aaDebug == 2) return float4(w, w, w, 1.0);

    float3 result = lerp(cM, blended, w);
    if (aaDebug == 3)
    {
        const float mid = aaContentOrigin.x + aaContentSize.x * 0.5;
        if (i.pos.x < mid) result = cM;   // A/B: left half unfiltered
    }
    return float4(result, 1.0);
}
