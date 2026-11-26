// Anti-aliasing pass 3: neighbourhood blend and diagnostics. Owner: shader-engineer.
//
// Reads the coverage weights and blends the pixel towards the neighbour across the strongest edge. Blending one side
// at a time (rather than averaging the whole 3x3 neighbourhood) keeps the texture on both sides of the edge intact:
// only the staircase end pixel moves, and it moves towards the sample it should have contained.
//
// Debug views (aaDebug): 1 EdgeMask (red = vertical edge, green = horizontal edge), 2 BlendWeight (max weight as grey),
// 3 Split (left half of the game image unfiltered).
#include "aa/aa_common.hlsli"

float4 main(AaVsOut i) : SV_Target
{
    const int2 p = int2(i.pos.xy);
    const float3 cM = AaLoad(p);
    if (!AaInsideContent(p)) return float4(cM, 1.0);   // letterbox: untouched, bit for bit

    const float4 w = weightTex.Load(int3(clamp(p, AaContentLo(), AaContentHi()), 0));
    const float m = max(max(w.r, w.g), max(w.b, w.a));

    if (aaDebug == 1)
    {
        const float2 e = edgeTex.Load(int3(clamp(p, AaContentLo(), AaContentHi()), 0)).rg;
        return float4(e.x, e.y, 0.0, 1.0);
    }
    if (aaDebug == 2) return float4(m, m, m, 1.0);

    float3 result = cM;
    if (m > 0.0001)
    {
        // At a 45-degree staircase corner two sides can carry the same weight. North/south are preferred on ties; the
        // two are equivalent for a corner, and a fixed order keeps the result deterministic.
        float3 cOther = cM;
        if (m == w.r)      cOther = AaLoad(p + int2( 0, -1));
        else if (m == w.g) cOther = AaLoad(p + int2( 0,  1));
        else if (m == w.b) cOther = AaLoad(p + int2(-1,  0));
        else               cOther = AaLoad(p + int2( 1,  0));
        result = lerp(cM, cOther, saturate(m));
    }

    if (aaDebug == 3)
    {
        const float mid = aaContentOrigin.x + aaContentSize.x * 0.5;
        if (i.pos.x < mid) result = cM;   // A/B: left half unfiltered
    }
    return float4(result, 1.0);
}
