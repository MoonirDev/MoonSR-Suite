// Anti-aliasing statistics: aliased-edge coverage on a coarse sample grid. Owner: shader-engineer.
// One thread per sample; each group reduces to two atomics. The CPU reads the counters a few frames later through a
// staging buffer (no stall), which drives the "already anti-aliased" reduction and the Home-page status.
//
// The test mirrors the gates of aa_spatial_ps.hlsl exactly (same threshold, same axis-alignment fade, same two-sided
// and along-edge tests, same 1 texel sampling distance), so coverage means "share of sampled pixels the filter would
// change" rather than just "has contrast". A checker texture, a one-pixel grid, a smooth gradient and text strokes read
// low; an aliased silhouette reads high. This is a detector, not a quality measure.
cbuffer AaStatsConstants : register(b0)
{
    float2 stTexSize;          // full source texture size in texels
    float2 stTexelSize;        // 1 / stTexSize
    float2 stContentOrigin;    // top-left of the game image inside the texture, texels
    float2 stContentSize;      // size of the game image, texels
    float  stEdgeThreshold;    // relative luma contrast that counts as an edge
    float  stEdgeThresholdMin; // absolute floor for dark scenes
    uint2  stGrid;             // samples per axis
};

Texture2D<float4> srcTex : register(t0);
SamplerState linearClamp : register(s0);

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
    return srcTex.Load(int3(clamp(p, lo, hi), 0)).rgb;
}

// Bilinear sample at a texel-space position, clamped to the game image (same mapping as AaSample in aa_common.hlsli).
float3 StSample(float2 pTexels)
{
    const float2 lo = stContentOrigin + 0.5;
    const float2 hi = stContentOrigin + stContentSize - 0.5;
    return srcTex.SampleLevel(linearClamp, clamp(pTexels, lo, hi) * stTexelSize, 0).rgb;
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
                // Long-edge coherence: the same side must continue two and four texels along the edge. A text stroke
                // or a small detail ends within that distance; a dense checker flips colour. This is what separates a
                // real silhouette from short high-frequency detail, and it is why the detector does not read a
                // text-heavy frame as "heavily aliased".
                const float lT3 = StLuma(StSample(pos + tangent * 2.0));
                const float lT4 = StLuma(StSample(pos - tangent * 2.0));
                const float lT5 = StLuma(StSample(pos + tangent * 4.0));
                const float lT6 = StLuma(StSample(pos - tangent * 4.0));
                const bool coherent = abs(lT3 - lM) <= 0.25 * range && abs(lT4 - lM) <= 0.25 * range &&
                                      abs(lT5 - lM) <= 0.25 * range && abs(lT6 - lM) <= 0.25 * range;
                // How far the centre sits from the two-side average: 0.5 at a hard one-pixel step, near 0 on an edge
                // that is already spread over several pixels (TAA, MSAA or a smooth gradient). Only hard steps count,
                // which is what makes this an "already anti-aliased" detector rather than an edge counter.
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
