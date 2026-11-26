// Anti-aliasing pass 1: edge detection and thin-stroke mask. Owner: shader-engineer.
//
// Writes RGBA8:
//   R = a vertical edge between this pixel and the one to its west,
//   G = a horizontal edge between this pixel and the one above it,
//   B = this pixel is a one-texel-wide stroke (text, railing, fine line),
//   A = unused (1).
//
// Only visible luminance steps become edges: the relative threshold plus an absolute floor for dark scenes, then a
// local-contrast adaptation (an edge must carry at least half the strongest delta in its 3x3 neighbourhood). The
// adaptation is the published SMAA improvement on plain MLAA: it stops a weak edge next to a strong one from being
// filtered, which is what keeps texture grain and soft shading out of the mask.
//
// The thin-stroke mask is computed here because this pass already has the four neighbours. It is sign-based: both
// neighbours on one axis must sit on the same side of the centre. That survives a textured background (a checker
// behind text) and does not fire on a straight half-plane edge, where one neighbour is on the centre's side. The
// weights pass dilates it by one texel and drops every coverage weight in the protected region, so a glyph cap or an
// arm/stem junction is protected as well as its straight sides.
//
// Written from the published description (Jimenez et al., "SMAA: Enhanced Subpixel Morphological Antialiasing", 2011;
// Reshetov, "Morphological Antialiasing", 2009). No third-party shader code was copied.
#include "aa/aa_common.hlsli"

float4 main(AaVsOut i) : SV_Target
{
    const int2 p = int2(i.pos.xy);
    if (!AaInsideContent(p)) return float4(0.0, 0.0, 0.0, 1.0);

    const float lM  = AaLuma(AaLoad(p));
    const float lW  = AaLuma(AaLoad(p + int2(-1,  0)));
    const float lN  = AaLuma(AaLoad(p + int2( 0, -1)));
    const float lE  = AaLuma(AaLoad(p + int2( 1,  0)));
    const float lS  = AaLuma(AaLoad(p + int2( 0,  1)));
    const float lWW = AaLuma(AaLoad(p + int2(-2,  0)));
    const float lNN = AaLuma(AaLoad(p + int2( 0, -2)));

    // Deltas across the two edges that meet at this pixel, and one texel further out. The maximum over the four
    // deltas is the local contrast used by the adaptation step.
    const float2 d0 = abs(float2(lM, lM) - float2(lW, lN));
    const float2 d1 = abs(float2(lM, lM) - float2(lE, lS));
    const float2 d2 = abs(float2(lW, lN) - float2(lWW, lNN));
    const float maxDelta = max(max(d0.x, d0.y), max(max(d1.x, d1.y), max(d2.x, d2.y)));

    const float localMax = max(lM, max(max(lW, lN), max(lE, lS)));
    const float threshold = max(aaEdgeThresholdMin, aaEdgeThreshold * localMax);

    float2 edges = step(threshold.xx, d0);
    // An edge is kept only if it is at least half the strongest local step: weak texture next to a silhouette is not
    // resolved by blending it, it only loses contrast.
    edges *= step(maxDelta.xx, 2.0 * d0);

    // One-texel stroke, in either axis. A 2 px feature does not match because its far neighbour is still the feature.
    const float dW = lW - lM, dE = lE - lM, dN = lN - lM, dS = lS - lM;
    const bool hThin = dW * dE > 0.0f && min(abs(dW), abs(dE)) > 0.25f * max(abs(dW), abs(dE)) &&
                       max(abs(dW), abs(dE)) > 0.02f;
    const bool vThin = dN * dS > 0.0f && min(abs(dN), abs(dS)) > 0.25f * max(abs(dN), abs(dS)) &&
                       max(abs(dN), abs(dS)) > 0.02f;

    return float4(edges.x, edges.y, (hThin || vThin) ? 1.0 : 0.0, 1.0);
}
