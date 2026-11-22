// Cross-direction candidates after the sub-block refinement (two entry points).
//
// A hierarchical search loses small fast objects: at the coarse levels they vanish, and the fine levels only search
// a few pixels around the coarse prediction. When one direction does find such an object (e.g. through the
// motion-compensated temporal candidate), the other direction can be told where to look: content at x in frame A
// with vector v lands at x + v in frame B, so the B cell there should have vector -v.
//
// Entry `splat` (one thread per cell, both directions): writes key = quality << 21 | source cell index into the
// OTHER direction's grid at the landing cell (InterlockedMax), quality = 1 + 1022 / (1 + 4 rel).
// Entry `main` (one thread per cell, both directions): if a key exists at the cell, the negated landed vector is a
// candidate. It replaces the refined vector only if its rms (6x6 window, as in the refinement) is lower by a margin
// (cost < 0.8 * current). Output: same layout as the refined field.
//   gSrcSize level-0 size   gDstSize cell grid size   gP0.x relative margin
#include "framegen/fg_common.hlsli"

Texture2D<float> gLumPrev : register(t0);
Texture2D<float> gLumCurr : register(t1);
Texture2DArray<float4> gRefined : register(t2);
Texture2DArray<uint> gKeysIn : register(t3);
SamplerState gLinear : register(s0);
RWTexture2DArray<uint> gKeys : register(u0);
RWTexture2DArray<float4> gOut : register(u1);

#define WIN 6
#define WIN2 (WIN * WIN)

[numthreads(8, 8, 1)]
void splat(uint3 id : SV_DispatchThreadID)
{
    const int2 cell = int2(id.xy);
    const uint dir = id.z;
    if (any(cell >= gDstSize)) return;
    const float4 r = gRefined.Load(int4(cell, dir, 0));
    const float rel = r.z / (r.w + 2.0);
    const uint q = 1u + uint(1022.0 / (1.0 + 4.0 * rel));
    const float2 land = (float2(cell) + 0.5) * float(kCell) + r.xy;
    const int2 lc = int2(floor(land / float(kCell)));
    if (any(lc < 0) || any(lc >= gDstSize)) return;
    const uint key = (q << 21) | (uint(cell.y * gDstSize.x + cell.x) & 0x1FFFFFu);
    InterlockedMax(gKeys[uint3(lc, 1u - dir)], key);
}

float Rms(Texture2D<float> src, Texture2D<float> ref, int2 org, float2 v)
{
    const float2 invSize = 1.0 / float2(gSrcSize);
    float sd = 0.0, sd2 = 0.0;
    [unroll] for (int i = 0; i < WIN2; ++i) {
        const int2 q = org + Unflatten(i, WIN);
        const float d = src.Load(int3(ClampI(q, gSrcSize), 0)) - ref.SampleLevel(gLinear, (float2(q) + 0.5 + v) * invSize, 0);
        sd += d;
        sd2 += d * d;
    }
    return sqrt(max(sd2 - sd * sd / float(WIN2), 0.0) / float(WIN2));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 cell = int2(id.xy);
    const uint dir = id.z;
    if (any(cell >= gDstSize)) return;
    float4 r = gRefined.Load(int4(cell, dir, 0));
    const uint key = gKeysIn.Load(int4(cell, dir, 0));
    if (key != 0) {
        const uint idx = key & 0x1FFFFFu;
        const int2 srcCell = int2(idx % uint(gDstSize.x), idx / uint(gDstSize.x));
        const float2 cand = -gRefined.Load(int4(srcCell, 1u - dir, 0)).xy;
        if (length(cand - r.xy) > 2.0) {
            const int2 org = cell * kCell - 1;
            float c;
            [branch] if (dir == 0) c = Rms(gLumPrev, gLumCurr, org, cand);
            else c = Rms(gLumCurr, gLumPrev, org, cand);
            if (c < gP0.x * r.z) r.xyz = float3(cand, c);
        }
    }
    gOut[uint3(cell, dir)] = r;
}
