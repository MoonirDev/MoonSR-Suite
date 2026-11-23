// Smart parallax analysis, guided-filter passes: self-guided filter of L (He, Sun, Tang, "Guided Image Filtering",
// ECCV 2010), computed at HALF resolution as in He & Sun, "Fast Guided Filter" (arXiv 1505.00996, 2015): the box
// statistics and the linear coefficients (a, b) are computed on the subsampled guide and the mean coefficients are
// upsampled bilinearly; the gate and height passes then evaluate B = mean_a * L + mean_b at full resolution.
// Owner: upscaling-engineer. Clean-room from the published algorithm:
//   mean_I, mean_II over a (2r+1)^2 box; var = mean_II - mean_I^2; a = var / (var + eps); b = (1 - a) * mean_I;
//   B = mean(a) * I + mean(b) (base, edge-preserving); D = I - B (detail = the relief source).
// One 2D box per dispatch (separable inside group shared memory), radius cRadius <= RMAX half texels:
//   RELIEF_BOX_PASS 1: I = 2x2 mean of L (one bilinear tap) -> box of (I, I^2) -> (a, b)
//   RELIEF_BOX_PASS 2: box of (a, b) -> (mean_a, mean_b)
// (Folding pass 2 into the gate pass was measured slower on the RTX 5060: 0.216 vs 0.200 ms Analyse at 1080p.)
// Borders clamp (replicate), which keeps the box mean unbiased at the frame edge.
#include "relief/relief_common.hlsli"

#ifndef RELIEF_BOX_PASS
#define RELIEF_BOX_PASS 1
#endif

#if RELIEF_BOX_PASS == 1
Texture2D<float>  inTex : register(t0); // full-size L
SamplerState linClamp   : register(s0);
#else
Texture2D<float2> inTex : register(t0); // half-size (a, b)
#endif
RWTexture2D<float2> outTex : register(u0);

#define TILE 16
#define RMAX 6
#define SIDE (TILE + 2 * RMAX)
groupshared float2 gsIn[SIDE * SIDE];
groupshared float2 gsRow[SIDE * TILE]; // horizontal sums: all SIDE rows, TILE columns

[numthreads(TILE, TILE, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
#ifdef RELIEF_BOX_RADIUS
    const int r = RELIEF_BOX_RADIUS;
#else
    const int r = clamp(cRadius, 1, RMAX);
#endif
    const int2 hi = int2(cHalf) - 1;
    // Only the halo the actual radius needs is loaded (side = TILE + 2r; the arrays are sized for RMAX).
    const uint side = uint(TILE + 2 * r);
    const int2 org = int2(gid.xy) * TILE - r;
    for (uint i = gi; i < side * side; i += TILE * TILE) {
        const int2 q = clamp(org + int2(i % side, i / side), int2(0, 0), hi);
#if RELIEF_BOX_PASS == 1
        const float v = LDec(inTex.SampleLevel(linClamp, (float2(q) * 2.0 + 1.0) / float2(cSize), 0));
        gsIn[i] = float2(v, v * v);
#else
        gsIn[i] = inTex.Load(int3(q, 0));
#endif
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = gi; j < side * TILE; j += TILE * TILE) {
        const int row = int(j / TILE), col = int(j % TILE) + r;
        float2 s = 0.0;
#ifdef RELIEF_BOX_RADIUS
        [unroll]
#endif
        for (int k = -r; k <= r; ++k) s += gsIn[row * int(side) + col + k];
        gsRow[j] = s;
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 p = int2(gid.xy) * TILE + int2(gtid.xy);
    if (any(p >= int2(cHalf))) return;
    float2 s = 0.0;
#ifdef RELIEF_BOX_RADIUS
    [unroll]
#endif
    for (int k = -r; k <= r; ++k) s += gsRow[(int(gtid.y) + r + k) * TILE + gtid.x];
    s /= float((2 * r + 1) * (2 * r + 1));
#if RELIEF_BOX_PASS == 1
    const float var = max(s.y - s.x * s.x, 0.0);
    const float a = var / (var + cEps);
    outTex[p] = float2(a, (1.0 - a) * s.x);
#else
    outTex[p] = s;
#endif
}
