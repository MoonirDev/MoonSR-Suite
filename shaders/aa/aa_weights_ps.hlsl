// Anti-aliasing pass 2: sub-pixel coverage weights. Owner: shader-engineer.
//
// For each pixel and each of its four edges, walk along the edge until the run ends or a crossing edge turns the
// staircase. The pixel gets a coverage weight only when it sits at one end of a run: weight = 0.5 / runLength. That is
// the area of the triangle cut off by a straight line that approximates the staircase over the run (a 45-degree step
// has run length 1 and weight 0.5; a 26.6-degree step has length 2 and weight 0.25). The middle of a straight run gets
// nothing: a long axis-aligned edge or a text stem is already resolved by the pixel grid.
//
// Output (RGBA8): R = blend towards north, G = south, B = west, A = east.
//
// Written from the published description of MLAA (Reshetov, Intel, 2009) and SMAA (Jimenez et al., 2011). No
// third-party shader code was copied.
#include "aa/aa_common.hlsli"

float EdgeH(int2 p)
{
    return edgeTex.Load(int3(clamp(p, AaContentLo(), AaContentHi()), 0)).g;
}

float EdgeV(int2 p)
{
    return edgeTex.Load(int3(clamp(p, AaContentLo(), AaContentHi()), 0)).r;
}

float ThinFlag(int2 p)
{
    return edgeTex.Load(int3(clamp(p, AaContentLo(), AaContentHi()), 0)).b;
}

// Distance, in edge pixels, from the pixel that OWNS the edge to the end of the horizontal run in `dir`
// (+1 right, -1 left). 0 means the run ends at that pixel. A crossing vertical edge ends the run where it is found.
int HorizontalRun(int2 anchor, int dir, int maxSteps)
{
    int d = 0;
    [loop]
    for (int k = 1; k <= maxSteps; ++k)
    {
        const int2 q = anchor + int2(dir * k, 0);
        if (!AaInsideContent(q) || EdgeH(q) < 0.5) break;
        d = k;
        if (EdgeV(q) > 0.5 || EdgeV(q + int2(1, 0)) > 0.5) break;
    }
    return d;
}

int VerticalRun(int2 anchor, int dir, int maxSteps)
{
    int d = 0;
    [loop]
    for (int k = 1; k <= maxSteps; ++k)
    {
        const int2 q = anchor + int2(0, dir * k);
        if (!AaInsideContent(q) || EdgeV(q) < 0.5) break;
        d = k;
        if (EdgeH(q) > 0.5 || EdgeH(q + int2(0, 1)) > 0.5) break;
    }
    return d;
}

// A one-pixel feature (a glyph stem, a railing, a thin highlight) has the background one texel beyond its far side.
// Blending the end of such a feature is the classic way a post-process AA destroys text and fine detail, so the
// weight is dropped while protection is on. Two-pixel features and wider are not matched: they are shapes, not
// strokes, and their staircases are what the filter exists to resolve.
bool ThinVertical(int2 left, int2 right, float contrast)
{
    const float lL = AaLuma(AaLoad(left));
    const float lR = AaLuma(AaLoad(right));
    const float lRR = AaLuma(AaLoad(right + int2(1, 0)));
    const float lLL = AaLuma(AaLoad(left + int2(-1, 0)));
    return abs(lRR - lL) < 0.25 * contrast || abs(lLL - lR) < 0.25 * contrast;
}

bool ThinHorizontal(int2 top, int2 bottom, float contrast)
{
    const float lT = AaLuma(AaLoad(top));
    const float lB = AaLuma(AaLoad(bottom));
    const float lBB = AaLuma(AaLoad(bottom + int2(0, 1)));
    const float lTT = AaLuma(AaLoad(top + int2(0, -1)));
    return abs(lBB - lT) < 0.25 * contrast || abs(lTT - lB) < 0.25 * contrast;
}

// Weight for an edge that runs horizontally. p is the pixel on this side; the edge is owned by `anchor`, the pixel
// whose top edge it is (p itself for a north edge, p + south for a south edge).
float HorizontalWeight(int2 p, bool north, int maxSteps)
{
    const int2 anchor = north ? p : p + int2(0, 1);
    if (EdgeH(anchor) < 0.5) return 0.0;
    const int dL = HorizontalRun(anchor, -1, maxSteps);
    const int dR = HorizontalRun(anchor,  1, maxSteps);
    if (dL > 0 && dR > 0) return 0.0;                    // interior of the run
    const int len = dL + dR + 1;
    float w = 0.5 / sqrt(float(len));
    if (aaProtectText != 0 || aaProtectHud != 0)
    {
        const int2 top = north ? p : p + int2(0, 1);
        const int2 bottom = north ? p + int2(0, 1) : p;
        const float contrast = abs(AaLuma(AaLoad(top)) - AaLuma(AaLoad(bottom)));
        if (contrast > 0.0 && ThinHorizontal(top, bottom, contrast)) w = 0.0;
    }
    return w;
}

float VerticalWeight(int2 p, bool west, int maxSteps)
{
    const int2 anchor = west ? p : p + int2(1, 0);
    if (EdgeV(anchor) < 0.5) return 0.0;
    const int dT = VerticalRun(anchor, -1, maxSteps);
    const int dB = VerticalRun(anchor,  1, maxSteps);
    if (dT > 0 && dB > 0) return 0.0;
    const int len = dT + dB + 1;
    float w = 0.5 / sqrt(float(len));
    if (aaProtectText != 0 || aaProtectHud != 0)
    {
        const int2 left = west ? p : p + int2(-1, 0);
        const int2 right = west ? p + int2(1, 0) : p;
        const float contrast = abs(AaLuma(AaLoad(right)) - AaLuma(AaLoad(left)));
        if (contrast > 0.0 && ThinVertical(left, right, contrast)) w = 0.0;
    }
    return w;
}

float4 main(AaVsOut i) : SV_Target
{
    const int2 p = int2(i.pos.xy);
    if (!AaInsideContent(p) || aaStrength <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);

    // Text and HUD protection: a one-texel stroke is a shape, not a staircase. Protecting it covers glyph caps and
    // arm/stem junctions, which the per-edge tests cannot see because those edges are not across the stroke. The mask
    // comes from the edge pass (B channel) and is dilated by one texel, so the corner pixel of a glyph and the
    // background touching it are protected too. Five small edge-texture loads, not twenty luma loads.
    if (aaProtectText != 0 || aaProtectHud != 0)
    {
        bool thin = ThinFlag(p) > 0.5;
        if (!thin)
        {
            const int2 n[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
            [unroll]
            for (int k = 0; k < 4; ++k)
                thin = thin || ThinFlag(p + n[k]) > 0.5;
        }
        if (thin) return float4(0.0, 0.0, 0.0, 0.0);
    }

    const int steps = AaSearchSteps();
    const float wN = HorizontalWeight(p, true, steps);
    const float wS = HorizontalWeight(p, false, steps);
    const float wW = VerticalWeight(p, true, steps);
    const float wE = VerticalWeight(p, false, steps);

    const float s = saturate(aaStrength);
    return float4(wN * s, wS * s, wW * s, wE * s);
}
