// Vector-field regularisation for one pyramid level, both directions. One thread group (64 threads) per block.
//
// Neighbour consensus with a data term: the 9 vectors of the 3x3 block neighbourhood are candidates for the centre
// block. Each candidate k is scored by
//     rms weighted zero-mean difference of the centre block matched with v_k (bilinear)
//   + mu * (1/8) * sum_j min(|v_k - v_j|_1, tau)        (truncated L1 to the neighbourhood, edge preserving)
// and the minimum wins (the block's own vector is preferred on ties). Unlike a plain vector median this keeps a
// small object whose own vector matches much better than the neighbours', and removes outliers whose match is
// only marginally better than the consensus. Correspondences are weighted by (1 - hud(p)) (1 - hud(p + v)) with the
// previous pair's HUD mask, as in the search.
// With gU0.x = step > 1 the same pass propagates vectors over longer range (candidates at +-step blocks; a few
// passes with decreasing step act like jump-flooding): a correct vector found in one part of a region with a
// repetitive pattern reaches the parts where the local search locked onto an alias, and wins there on data cost.
//   gSrcSize level size     gDstSize block grid size
//   gP0.x mu   gP0.y tau (level px)   gU0.x neighbour step (blocks)   gU1.x use HUD weights
#include "framegen/fg_common.hlsli"

Texture2D<float> gLumPrev : register(t0);
Texture2D<float> gLumCurr : register(t1);
Texture2DArray<float4> gField : register(t2);
Texture2D<float> gHud : register(t3);
SamplerState gLinear : register(s0);
RWTexture2DArray<float4> gOut : register(u0);

groupshared float2 sV[9];
groupshared float sD[9 * 64];
groupshared float sW[64];
groupshared float sDW[9 * 64];
groupshared float sCost[9];
groupshared float sWeightSum;
groupshared float3 sPartial[9 * 7];

void Regularize(Texture2D<float> src, Texture2D<float> ref, uint3 gid, uint tid)
{
    const uint dir = gid.z;
    const int2 block = int2(gid.xy);
    const int step = int(max(gU0.x, 1u));
    if (tid < 9) sV[tid] = gField.Load(int4(ClampI(block + (Unflatten(int(tid), 3) - 1) * step, gDstSize), dir, 0)).xy;
    const int2 p = block * kBlock + Unflatten(int(tid), 8);
    const float2 invSize = 1.0 / float2(gSrcSize);
    sW[tid] = gU1.x != 0 ? 1.0 - gHud.SampleLevel(gLinear, (float2(p) + 0.5) * invSize, 0) : 1.0;
    GroupMemoryBarrierWithGroupSync();

    const float s = src.Load(int3(ClampI(p, gSrcSize), 0));
    [unroll] for (uint k = 0; k < 9; ++k) {
        const float2 uv = (float2(p) + 0.5 + sV[k]) * invSize;
        sD[k * 64 + tid] = s - ref.SampleLevel(gLinear, uv, 0);
        sDW[k * 64 + tid] = gU1.x != 0 ? 1.0 - gHud.SampleLevel(gLinear, uv, 0) : 1.0;
    }
    if (tid < 8) {
        float partial = 0.0;
        [unroll] for (uint i = 0; i < 8; ++i) partial += sW[tid * 8 + i];
        sPartial[tid].x = partial;
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) {
        float sum = 0.0;
        [unroll] for (uint i = 0; i < 8; ++i) sum += sPartial[i].x;
        sWeightSum = sum;
    }
    GroupMemoryBarrierWithGroupSync();

    // Share each candidate's reduction across seven lanes. Previously only nine of the 64 lanes did all
    // 64 samples serially, leaving most of each group idle during this relatively expensive pass.
    const bool uniformW = sWeightSum < 16.0;
    const float wsum = uniformW ? 64.0 : sWeightSum;
    if (tid < 63) {
        const uint candidate = tid / 7;
        const uint part = tid % 7;
        float sd = 0.0, sd2 = 0.0, sw = 0.0;
        for (uint i = part; i < 64; i += 7) {
            const float w = uniformW ? 1.0 : sW[i] * sDW[candidate * 64 + i];
            const float d = sD[candidate * 64 + i];
            sd += w * d;
            sd2 += w * d * d;
            sw += w;
        }
        sPartial[tid] = float3(sd, sd2, sw);
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid < 9) {
        float3 sums = 0.0;
        [unroll] for (uint part = 0; part < 7; ++part) sums += sPartial[tid * 7 + part];
        const float sd = sums.x, sd2 = sums.y, sw = sums.z;
        float smooth = 0.0;
        for (uint j = 0; j < 9; ++j) smooth += min(dot(abs(sV[tid] - sV[j]), 1.0), gP0.y);
        const float data = sw >= 0.25 * wsum ? sqrt(max(sd2 - sd * sd / sw, 0.0) / sw) : 1e4;
        sCost[tid] = data + gP0.x * smooth / 8.0;
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid == 0) {
        uint best = 4;
        float bestC = sCost[4];
        for (uint k2 = 0; k2 < 9; ++k2) {
            if (sCost[k2] < bestC) {
                bestC = sCost[k2];
                best = k2;
            }
        }
        if (all(block < gDstSize)) gOut[uint3(gid.xy, dir)] = float4(sV[best], bestC, 0.0);
    }
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex)
{
    [branch] if (gid.z == 0) Regularize(gLumPrev, gLumCurr, gid, tid);
    else Regularize(gLumCurr, gLumPrev, gid, tid);
}
