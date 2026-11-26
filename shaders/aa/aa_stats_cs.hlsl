// Anti-aliasing statistics: hard-edge density on a coarse sample grid. Owner: shader-engineer.
//
// One thread per sample; each group reduces to two atomics. The CPU reads the counters a few frames later through a
// staging ring (no stall). The figure is a density of long, slanted, hard one-pixel steps, not a quality score: text,
// dense texture and already anti-aliased content should not read as aliased, and the number never changes the filter
// strength. See documentation/research/anti_aliasing.md section 2.7 and KNOWN_ISSUES.md.
//
// The test is deliberately stricter than the filter gates: the step must be two-sided, the two sides must continue 2 and
// 4 texels along the edge (a glyph or a small detail ends inside that distance, a checker flips colour), and the centre
// must sit at least a quarter of the contrast away from the two-side average (a step already spread over two pixels is
// resolved). That last term is the closest cheap proxy for "already anti-aliased"; it is reported, not acted on.
cbuffer AaStatsConstants : register(b0)
{
    float2 stTexSize;          // full texture size in texels
    float2 stTexelSize;        // 1 / stTexSize
    float2 stContentOrigin;    // top-left of the game image inside the texture, texels
    float2 stContentSize;      // size of the game image, texels
    float  stEdgeThreshold;    // relative luma contrast
    float  stEdgeThresholdMin; // absolute floor
    uint2  stGrid;             // samples per axis
};

Texture2D<float4> stSrcTex : register(t0);
SamplerState stLinearClamp : register(s0);

// [0] = samples classified as an aliased edge, [1] = samples taken.
RWStructuredBuffer<uint> counter : register(u0);

groupshared uint gEdge;
groupshared uint gTotal;

float StLuma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

float3 StLoad(int2 p)
{
    const int2 lo = int2(stContentOrigin);
    const int2 hi = int2(stContentOrigin + stContentSize) - 1;
    return stSrcTex.Load(int3(clamp(p, lo, hi), 0)).rgb;
}

float3 StSample(float2 pTexels)
{
    const float2 lo = stContentOrigin + 0.5;
    const float2 hi = stContentOrigin + stContentSize - 0.5;
    return stSrcTex.SampleLevel(stLinearClamp, clamp(pTexels, lo, hi) * stTexelSize, 0).rgb;
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    if (tid.x == 0 && tid.y == 0) { gEdge = 0; gTotal = 0; }
    GroupMemoryBarrierWithGroupSync();

    const uint2 idx = gid.xy * 8u + tid.xy;
    if (all(idx < stGrid))
    {
        const float2 uv = (float2(idx) + 0.5) / float2(stGrid);
        const int2 p = int2(stContentOrigin + uv * stContentSize);

        const float lM = StLuma(StLoad(p));
        const float lN = StLuma(StLoad(p + int2( 0, -1)));
        const float lS = StLuma(StLoad(p + int2( 0,  1)));
        const float lW = StLuma(StLoad(p + int2(-1,  0)));
        const float lE = StLuma(StLoad(p + int2( 1,  0)));
        const float lNW = StLuma(StLoad(p + int2(-1, -1)));
        const float lNE = StLuma(StLoad(p + int2( 1, -1)));
        const float lSW = StLuma(StLoad(p + int2(-1,  1)));
        const float lSE = StLuma(StLoad(p + int2( 1,  1)));

        const float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
        const float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
        const float range = lMax - lMin;
        const float threshold = max(stEdgeThresholdMin, lMax * stEdgeThreshold);

        bool edge = false;
        if (range >= threshold)
        {
            const float gx = (lNE + 2.0 * lE + lSE) - (lNW + 2.0 * lW + lSW);
            const float gy = (lSW + 2.0 * lS + lSE) - (lNW + 2.0 * lN + lNE);
            const float g1 = abs(gx) + abs(gy) + 1e-4;
            const float2 across = float2(gx, gy) / g1;
            const float axisAlign = max(abs(gx), abs(gy)) / g1;
            if (axisAlign < 0.92)
            {
                const float2 pos = float2(p) + 0.5;
                const float lPos = StLuma(StSample(pos + across));
                const float lNeg = StLuma(StSample(pos - across));
                const float2 tangent = float2(-across.y, across.x);
                const float lT1 = StLuma(StSample(pos + tangent));
                const float lT2 = StLuma(StSample(pos - tangent));
                const bool twoSided = abs(lPos - lNeg) >= threshold;
                const bool between = lM >= min(lPos, lNeg) - 0.05 * range &&
                                     lM <= max(lPos, lNeg) + 0.05 * range;
                const bool alongOk = abs(lT1 - lM) <= 0.5 * range && abs(lT2 - lM) <= 0.5 * range;
                const float lT3 = StLuma(StSample(pos + tangent * 2.0));
                const float lT4 = StLuma(StSample(pos - tangent * 2.0));
                const float lT5 = StLuma(StSample(pos + tangent * 4.0));
                const float lT6 = StLuma(StSample(pos - tangent * 4.0));
                const bool coherent = abs(lT3 - lM) <= 0.25 * range && abs(lT4 - lM) <= 0.25 * range &&
                                      abs(lT5 - lM) <= 0.25 * range && abs(lT6 - lM) <= 0.25 * range;
                const float sideContrast = abs(lPos - lNeg);
                const float dev = abs(lM - (lPos + lNeg) * 0.5) / max(sideContrast, 1e-4);
                edge = twoSided && between && alongOk && coherent && dev >= 0.25;
            }
        }
        if (edge) InterlockedAdd(gEdge, 1u);
        InterlockedAdd(gTotal, 1u);
    }

    GroupMemoryBarrierWithGroupSync();
    if (tid.x == 0 && tid.y == 0)
    {
        InterlockedAdd(counter[0], gEdge);
        InterlockedAdd(counter[1], gTotal);
    }
}
