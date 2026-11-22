// Hierarchical block matching, one pyramid level, both directions (SV_GroupID.z: 0 = prev->curr, 1 = curr->prev).
// One thread group per 8x8 block.
//
// 1. Prediction: up to 11 candidate vectors are scored with a bilinear zero-mean SSD over the block: the upsampled
//    vectors (x2) of the 3x3 parent blocks of the coarser level, the zero vector, and the previous pair's final vector
//    at this location (temporal candidate). The best one is the prediction p.
// 2. Search: exhaustive integer search in a (2r+1)^2 window around round(p). The reference window (with a 1-pixel
//    border) and the source block are staged in groupshared memory. Cost per offset:
//        rms(zero-mean difference)  [robust to additive brightness change]
//      + lambdaC * mean Hamming distance of 8-neighbour census signatures  [robust to gain / monotonic change]
//      + lambdaS * L1 distance from the prediction  [tie-break toward the prediction; regularises flat/periodic areas]
// 3. Repeating-texture disambiguation (gP1.y > 0): of all offsets whose cost is within near = gP1.y + gP1.z * best of
//    the best, the one closest to the prediction is taken, then descended to the bottom of its own cost valley
//    (steepest descent over the 8 neighbours). On a periodic pattern (fence, stairs, tiles) every period is a near-equal
//    minimum; the plain arg-min picks one by noise. Technique from the owner's DLSS-FG addon FlowEstimator
//    (LS Addons/DLSS-FG/src/engine/flow_estimator.cpp, derived from the MIT-licensed LS Addon Manager); this is a
//    re-implementation in MoonSR's search, not a copy.
// 4. Sub-pixel: SubPixel() on the rms of the chosen offset and its 4 neighbours (exact for bilinear shifts).
//
// Overlay exclusion: each correspondence (source pixel p, reference pixel p + v) is weighted by
// (1 - hud(p)) * (1 - hud(p + v)) with the HUD mask of the previous pair, in every cost term. A static overlay inside
// a block then neither pulls the block's vector toward zero nor penalises the true vector where the overlay covers
// the reference. A candidate whose remaining weight is below a quarter of the block's is rejected. If the block is
// mostly overlay (source weight sum < 16 of 64), uniform weights are used (the overlay's zero motion is right then).
//
//   gSrcSize   level size                  gDstSize   block grid size of this level
//   gFieldSize coarser block grid size     gAuxSize   temporal field (cell grid) size
//   gP0.x lambdaS   gP0.y lambdaC   gP0.z frame px -> level px factor   gP0.w census threshold (levels)
//   gP1.x level px -> cell index factor (for the temporal lookup)   gP1.yz near-best margin (absolute, relative)
//   gU0.x radius r (<= MAX_R)   gU0.y has coarser   gU0.z has temporal   gU0.w use census   gU1.x use HUD weights
//   gU1.z parent candidates (4 or 9)
// Output (u0, slice = direction): float4(vector in level px, cost, 0).
#include "framegen/fg_common.hlsli"

Texture2D<float> gLumPrev : register(t0);
Texture2D<float> gLumCurr : register(t1);
Texture2DArray<float4> gCoarse : register(t2);
Texture2DArray<float4> gTemporal : register(t3);
Texture2D<float> gHud : register(t4);
SamplerState gLinear : register(s0);
RWTexture2DArray<float4> gOut : register(u0);

// Groupshared storage is sized for MAX_R; it limits occupancy, so the build compiles a MAX_R=4 variant for the
// fine levels and the MAX_R=8 default for the coarsest level (see shaders/CMakeLists.txt).
#ifndef MAX_R
#define MAX_R 8
#endif
#ifndef SEARCH_CENSUS
#define SEARCH_CENSUS 1
#endif
#define WIN_MAX (kBlock + 2 * MAX_R + 2)
#define CWIN_MAX (kBlock + 2 * MAX_R)
#define NCAND_MAX ((2 * MAX_R + 1) * (2 * MAX_R + 1))

groupshared float sSrc[100]; // 10x10: block plus 1-pixel border
groupshared uint sSrcC[64];  // census of the block
groupshared float sW[64];    // per-pixel weights
groupshared float sRef[WIN_MAX * WIN_MAX];
groupshared float sRefW[WIN_MAX * WIN_MAX];
groupshared uint sNonUnitWeight;
groupshared uint sRefC[CWIN_MAX * CWIN_MAX];
groupshared float sCost[NCAND_MAX];
groupshared float sSsd[NCAND_MAX];
groupshared float sPredCost[12];
groupshared float3 sPredPart[60]; // (sum w d, sum w d^2, sum w) partials: 12 candidates x 5 threads
groupshared float2 sPredVec[12];
groupshared uint sBest;
groupshared uint sNear;
groupshared float2 sPred;
groupshared float sWSum;

uint Census(float c, float n0, float n1, float n2, float n3, float n4, float n5, float n6, float n7, float thr)
{
    const float t = c + thr;
    return (n0 > t ? 1u : 0u) | (n1 > t ? 2u : 0u) | (n2 > t ? 4u : 0u) | (n3 > t ? 8u : 0u) |
           (n4 > t ? 16u : 0u) | (n5 > t ? 32u : 0u) | (n6 > t ? 64u : 0u) | (n7 > t ? 128u : 0u);
}

// Partial weighted zero-mean SSD terms of the source block against `ref` displaced by v (bilinear), over the pixels
// part, part + 5, ... (5 threads share one candidate).
float3 PredPartial(Texture2D<float> ref, int2 origin, float2 v, uint part, bool refWeights)
{
    const float2 invSize = 1.0 / float2(gSrcSize);
    float3 acc = float3(0, 0, 0);
    [loop] for (uint i = part; i < 64; i += 5) {
        const int2 q = Unflatten(int(i), 8);
        const float2 uv = (float2(origin + q) + 0.5 + v) * invSize;
        float w = sW[i];
        if (refWeights) w *= 1.0 - gHud.SampleLevel(gLinear, uv, 0);
        const float d = sSrc[(q.y + 1) * 10 + q.x + 1] - ref.SampleLevel(gLinear, uv, 0);
        acc += float3(w * d, w * d * d, w);
    }
    return acc;
}

void Search(Texture2D<float> src, Texture2D<float> ref, uint3 gid, uint tid)
{
    const uint dir = gid.z;
    const int2 block = int2(gid.xy);
    const int2 origin = block * kBlock;
    const int r = int(gU0.x);
    const float lambdaS = gP0.x;
    const float lambdaC = gU0.w != 0 ? gP0.y : 0.0;
    const float thr = gP0.w;

    // Source block with border, weights.
    for (uint i = tid; i < 100; i += 64) sSrc[i] = src.Load(int3(ClampI(origin + Unflatten(int(i), 10) - 1, gSrcSize), 0));
    const int2 lp = Unflatten(int(tid), 8);
    float w = 1.0;
    if (gU1.x != 0) w = 1.0 - gHud.SampleLevel(gLinear, (float2(origin + lp) + 0.5) / float2(gSrcSize), 0);
    sW[tid] = w;
    if (tid == 0) {
        sBest = 0xFFFFFFFFu;
        sNear = 0xFFFFFFFFu;
        sWSum = 0.0;
        sNonUnitWeight = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid < 8) {
        float partial = 0.0;
        [unroll] for (uint k = 0; k < 8; ++k) partial += sW[tid * 8 + k];
        sPredPart[tid].x = partial;
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) {
        float s = 0.0;
        [unroll] for (uint k = 0; k < 8; ++k) s += sPredPart[k].x;
        sWSum = s;
    }
    GroupMemoryBarrierWithGroupSync();
    const bool uniformW = sWSum < 16.0;
    const float wsum = uniformW ? 64.0 : sWSum;
    sW[tid] = uniformW ? 1.0 : w;
    if (sW[tid] != 1.0) InterlockedOr(sNonUnitWeight, 1u);
    GroupMemoryBarrierWithGroupSync();

    // ---- 1. prediction ----
    // Parent candidates: all 3x3 parents (gU1.z = 9), or the own parent and the three parents on the side of the
    // parent block this block lies in (gU1.z = 4).
    const uint nParents = gU1.z == 9 ? 9u : 4u;
    uint nPred = 1; // candidate 0 = zero vector
    if (gU0.y != 0) nPred += nParents;
    const uint tempIdx = nPred;
    if (gU0.z != 0) nPred += 1;
    {
        // 5 threads per candidate (up to 12 candidates -> 60 threads), partial sums combined below.
        const uint k = tid / 5u, part = tid - k * 5u;
        if (k < nPred) {
            float2 v = float2(0, 0);
            if (gU0.y != 0 && k >= 1 && k <= nParents) {
                int2 off;
                if (nParents == 9) off = Unflatten(int(k) - 1, 3) - 1;
                else {
                    const int2 side = int2((gid.x & 1u) != 0 ? 1 : -1, (gid.y & 1u) != 0 ? 1 : -1);
                    off = k == 1 ? int2(0, 0) : (k == 2 ? int2(side.x, 0) : (k == 3 ? int2(0, side.y) : side));
                }
                const int2 pb = ClampI(int2(gid.xy / 2) + off, gFieldSize);
                v = gCoarse.Load(int4(pb, dir, 0)).xy * 2.0;
            } else if (gU0.z != 0 && k == tempIdx) {
                // Temporal candidate, motion compensated for direction 0: the previous pair's curr->prev field lives
                // on the grid of this pair's prev frame, so -F10_prev(p) is the forward motion of the content now at
                // p under constant velocity (it follows a moving object; F01_prev(p) would describe whatever was at
                // p one frame earlier). Direction 1 uses F10_prev(p) (exact for global motion only).
                const float2 centreLevel = float2(origin) + 4.0;
                const int2 cell = ClampI(int2(centreLevel * gP1.x), gAuxSize);
                const float2 f10 = gTemporal.Load(int4(cell, 1, 0)).xy;
                v = (dir == 0 ? -f10 : f10) * gP0.z;
            }
            sPredPart[tid] = PredPartial(ref, origin, v, part, gU1.x != 0 && !uniformW);
            if (part == 0) sPredVec[k] = v;
        }
    }
    // Census of the source block (inner 8x8), independent of the prediction.
#if SEARCH_CENSUS
    {
        const int b = (lp.y + 1) * 10 + lp.x + 1;
        sSrcC[tid] = Census(sSrc[b], sSrc[b - 11], sSrc[b - 10], sSrc[b - 9], sSrc[b - 1], sSrc[b + 1], sSrc[b + 9],
                            sSrc[b + 10], sSrc[b + 11], thr);
    }
#endif
    GroupMemoryBarrierWithGroupSync();
    if (tid < nPred) {
        const float3 a = sPredPart[tid * 5] + sPredPart[tid * 5 + 1] + sPredPart[tid * 5 + 2] + sPredPart[tid * 5 + 3] + sPredPart[tid * 5 + 4];
        sPredCost[tid] = a.z < 0.25 * wsum ? 1e4 : sqrt(max(a.y - a.x * a.x / a.z, 0.0) / a.z);
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) {
        // Order of preference on ties: own parent, then the rest in index order.
        uint best = (gU0.y != 0) ? (nParents == 9 ? 5u : 1u) : 0u;
        float bestC = sPredCost[best];
        for (uint k = 0; k < nPred; ++k) {
            if (sPredCost[k] < bestC) {
                bestC = sPredCost[k];
                best = k;
            }
        }
        sPred = sPredVec[best];
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- 2. window search ----
    const float2 pred = sPred;
    const int2 pInt = int2(round(pred));
    const int win = kBlock + 2 * r + 2;
    const int cwin = kBlock + 2 * r;
    const int2 wOrigin = origin + pInt - r - 1;
    for (int j = int(tid); j < win * win; j += 64) {
        const int2 q = ClampI(wOrigin + Unflatten(j, win), gSrcSize);
        sRef[j] = ref.Load(int3(q, 0));
        sRefW[j] = (gU1.x != 0 && !uniformW) ? 1.0 - gHud.SampleLevel(gLinear, (float2(q) + 0.5) / float2(gSrcSize), 0) : 1.0;
        if (sRefW[j] != 1.0) InterlockedOr(sNonUnitWeight, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
#if SEARCH_CENSUS
    {
        for (int j2 = int(tid); j2 < cwin * cwin; j2 += 64) {
            const int2 q2 = Unflatten(j2, cwin);
            const int b = (q2.y + 1) * win + q2.x + 1;
            sRefC[j2] = Census(sRef[b], sRef[b - win - 1], sRef[b - win], sRef[b - win + 1], sRef[b - 1], sRef[b + 1],
                               sRef[b + win - 1], sRef[b + win], sRef[b + win + 1], thr);
        }
    }
#endif
    GroupMemoryBarrierWithGroupSync();

    const int side = 2 * r + 1;
    const int nCand = side * side;
    for (int c = int(tid); c < nCand; c += 64) {
        const int2 o = Unflatten(c, side); // offset + r
        float sd = 0.0, sd2 = 0.0, ham = 0.0, sw = 0.0;
        [branch] if (sNonUnitWeight == 0) {
            [loop] for (int uy = 0; uy < 8; ++uy) {
                const int urowR = (uy + o.y + 1) * win + o.x + 1;
                const int urowC = (uy + o.y) * cwin + o.x;
                [unroll] for (int ux = 0; ux < 8; ++ux) {
                    const float ud = sSrc[(uy + 1) * 10 + ux + 1] - sRef[urowR + ux];
                    sd += ud; sd2 += ud * ud;
#if SEARCH_CENSUS
                    ham += float(countbits(sSrcC[uy * 8 + ux] ^ sRefC[urowC + ux]));
#endif
                }
            }
            sw = 64.0;
        } else {
        [loop] for (int y = 0; y < 8; ++y) {
            const int rowR = (y + o.y + 1) * win + o.x + 1;
            const int rowC = (y + o.y) * cwin + o.x;
            [unroll] for (int x = 0; x < 8; ++x) {
                const float wk = sW[y * 8 + x] * sRefW[rowR + x];
                const float d = sSrc[(y + 1) * 10 + x + 1] - sRef[rowR + x];
                sd += wk * d;
                sd2 += wk * d * d;
#if SEARCH_CENSUS
                ham += wk * float(countbits(sSrcC[y * 8 + x] ^ sRefC[rowC + x]));
#endif
                sw += wk;
            }
        }
        }
        const bool valid = sw >= 0.25 * wsum;
        sw = max(sw, 1e-3);
        const float ssd = max(sd2 - sd * sd / sw, 0.0) / sw;
        const float2 v = float2(pInt + o - r);
        const float cost = sqrt(ssd) + lambdaC * ham / sw + lambdaS * dot(abs(v - pred), 1.0) + (valid ? 0.0 : 1e4);
        sCost[c] = cost;
        sSsd[c] = ssd;
        InterlockedMin(sBest, (asuint(cost) & 0xFFFFFE00u) | uint(c));
    }
    GroupMemoryBarrierWithGroupSync();

    [branch] if (gP1.y > 0.0) {
        const float bestCost = asfloat(sBest & 0xFFFFFE00u);
        const float nearT = bestCost + gP1.y + gP1.z * bestCost;
        for (int c2 = int(tid); c2 < nCand; c2 += 64) {
            if (sCost[c2] > nearT) continue;
            const float2 v = float2(pInt + Unflatten(c2, side) - r);
            const uint d = min(uint(dot(abs(v - pred), 1.0) * 16.0), 0x7FFFFFu);
            InterlockedMin(sNear, (d << 9) | uint(c2));
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid == 0) {
        int c = int(sBest & 0x1FFu);
        if (gP1.y > 0.0 && sNear != 0xFFFFFFFFu) {
            c = int(sNear & 0x1FFu);
            [loop] for (int step = 0; step < 2 * MAX_R; ++step) {
                const int2 oc = Unflatten(c, side);
                int next = c;
                float nc = sCost[c];
                [unroll] for (int k = 0; k < 9; ++k) {
                    const int2 on = oc + int2(k % 3, k / 3) - 1;
                    if (k == 4 || any(on < 0) || any(on >= side)) continue;
                    const int cn = on.y * side + on.x;
                    if (sCost[cn] < nc) {
                        nc = sCost[cn];
                        next = cn;
                    }
                }
                if (next == c) break;
                c = next;
            }
        }
        const int2 o = Unflatten(c, side);
        float2 sub = float2(0, 0);
        const float c0 = sqrt(sSsd[c]);
        if (o.x > 0 && o.x < side - 1) sub.x = SubPixel(c0, sqrt(sSsd[c - 1]), sqrt(sSsd[c + 1]));
        if (o.y > 0 && o.y < side - 1) sub.y = SubPixel(c0, sqrt(sSsd[c - side]), sqrt(sSsd[c + side]));
        const float2 v = float2(pInt + o - r) + sub;
        if (all(block < gDstSize)) gOut[uint3(gid.xy, dir)] = float4(v, sCost[c], 0.0);
    }
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex)
{
    // Direction 0 matches prev blocks in curr, direction 1 curr blocks in prev. gid.z is uniform per group.
    [branch] if (gid.z == 0) Search(gLumPrev, gLumCurr, gid, tid);
    else Search(gLumCurr, gLumPrev, gid, tid);
}
