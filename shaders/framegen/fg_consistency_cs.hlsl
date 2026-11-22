// Forward-backward consistency, confidence and pair statistics. One thread per cell, both directions.
//
// For a cell with vector v (direction d), the opposite field is read (nearest cell) where the cell lands:
//     e = |v + v_opposite(centre + v)|                consistency = exp(-0.5 (e / (tau0 + tauK |v|))^2)
// A landing point outside the image gives consistency 0.5 (cannot be verified).
//     rel = rms / (stddev + 2)                        match = 1 - smoothstep(relLo, relHi, rel)
//     confidence = consistency * match
// rel is ~0 for a good match and ~1.4 for unrelated content of the same contrast, independent of texture strength.
//   gSrcSize level-0 size   gDstSize cell grid size
//   gP0 = (tau0, tauK, flow px -> frame px, textured threshold)   gP1 = (relLo, relHi, bad rel threshold, 0)
// Output (slice = direction): float4(vector in FRAME px, confidence, rel, 0). Stats accumulate into u1.
#include "framegen/fg_common.hlsli"

Texture2DArray<float4> gRefined : register(t0);
RWTexture2DArray<float4> gOut : register(u0);
RWBuffer<uint> gStats : register(u1);

groupshared uint sConf, sMotion, sCount, sTextured, sBad;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0) {
        sConf = 0;
        sMotion = 0;
        sCount = 0;
        sTextured = 0;
        sBad = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 cell = int2(id.xy);
    const uint dir = id.z;
    if (all(cell < gDstSize)) {
        const float4 r = gRefined.Load(int4(cell, dir, 0));
        const float2 v = r.xy;
        const float2 land = (float2(cell) + 0.5) * float(kCell) + v;
        float consistency = 0.5;
        if (all(land >= 0.0) && all(land < float2(gSrcSize))) {
            const int2 lc = ClampI(int2(land / float(kCell)), gDstSize);
            const float2 vo = gRefined.Load(int4(lc, 1u - dir, 0)).xy;
            const float e = length(v + vo);
            const float tau = gP0.x + gP0.y * length(v);
            consistency = exp(-0.5 * (e / tau) * (e / tau));
        }
        const float rel = r.z / (r.w + 2.0);
        const float match = 1.0 - smoothstep(gP1.x, gP1.y, rel);
        const float conf = consistency * match;
        const float2 vf = v * gP0.z;
        gOut[uint3(cell, dir)] = float4(vf, conf, rel);

        if (dir == 0) {
            InterlockedAdd(sConf, uint(conf * 1024.0 + 0.5));
            InterlockedAdd(sMotion, uint(min(length(vf), 4096.0) * 16.0 + 0.5));
            InterlockedAdd(sCount, 1u);
        }
        if (r.w > gP0.w) {
            InterlockedAdd(sTextured, 1u);
            // "Bad" for the scene-cut statistic: a poor best match, or a vector the other direction contradicts. The
            // second cue keeps the statistic independent of the cell size: at flow scale 1 the 4x4-pixel cells of
            // unrelated smooth texture often find a passable match by chance, but not a consistent one.
            if (rel > gP1.z || consistency < 0.25) InterlockedAdd(sBad, 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        if (sCount != 0) {
            InterlockedAdd(gStats[kStatSumConf], sConf);
            InterlockedAdd(gStats[kStatSumMotion], sMotion);
            InterlockedAdd(gStats[kStatCount], sCount);
        }
        if (sTextured != 0) {
            InterlockedAdd(gStats[kStatTextured], sTextured);
            InterlockedAdd(gStats[kStatBad], sBad);
        }
    }
}
