// Moon ALSR pass 1 (motion-cell resolution, COMPUTE): sub-pixel refinement of the estimator's curr -> prev cell
// vectors. Owner: upscaling-engineer. Dispatch(ceil(cells.w / 2), ceil(cells.h / 2), 1): one 64-thread group per
// 2 x 2 cells, 16 threads ("lanes") per cell, each lane handles one column (16 samples) of the cell's 16 x 16 window.
// Output u0 (RGBA32F) = float4(v, confidence, track age), see below.
//
// Stage 3 cost cut (RTX 5060, 720p input, 14400 cells: 0.48 ms -> see alsr_progress.md): the pass was latency-bound -
// each cell is a serial chain (fetch, reduce, solve, fetch, ...) and only 2048 threads fit on an SM, so fewer threads
// per cell means more cells in flight. 256 -> 16 threads per cell; the six v-independent sums are reduced together
// (raw moments; the centring below is the same Schur complement as before); with the generator's default cell size
// (8 frame px) the window samples are exactly texel centres and the gradient taps their neighbours, so the current
// luma is read once per texel into a 26 x 26 groupshared tile shared by the group's 4 cells (bilinear fetch at a texel
// centre = that texel: identical values). Other cell sizes fetch directly (same results, slower). Same algorithm and
// window as before; only the float summation order differs.
//
// Algorithm (unchanged since the pixel-shader version alsr_cellrefine_ps of Stage 2):
//   window: 16 x 16 samples spaced cellPx / 8 around the cell centre (two cells wide), Gaussian weights
//           (sigma = cellPx / 2), all fetches bilinear,
//   model:  L_prev(x + v + dv) = L_curr(x) + o, L_prev = the registration reference built from the previous history
//           (alsr_refluma), L_curr = the blurred luma of the current frame (alsr_prep); the brightness offset o is
//           eliminated by centring the normal equations on their weighted means (Schur complement of the 3x3 system),
//   gradients of the current frame by central differences (+-1 px), Tikhonov term 2e-5 x sum(w), each step limited
//   to 1 px, gU2.w Gauss-Newton steps (default 2).
// The current-frame terms (w, gradients, the 2x2 matrix) do not depend on v: they are reduced once; each step then
// fetches one reference sample per window sample and reduces three sums.
// Velocity filter: LK starts from the previous frame's refined vector of the same content (within 2 px of the
// estimator's vector, otherwise the estimator's), and the result is blended with that prediction (see the end).
// Confidence is the estimator's cell confidence (forward-backward consistency x match quality).
// References: Lucas & Kanade 1981; Baker & Matthews, "Lucas-Kanade 20 years on", IJCV 2004 (offset elimination).
#include "upscale/alsr_common.hlsli"

Texture2D<float4> gWorkCurr : register(t0);
Texture2D<float4> gWorkPrev : register(t1);
Texture2DArray<float4> gFlow : register(t2);
Texture2D<float> gRef : register(t3);        // registration reference from the previous history (alsr_refluma)
Texture2D<float4> gPrevCells : register(t4); // this pass's output for the previous pair (curr -> prev of that pair)
Buffer<uint> gStats : register(t5);
RWTexture2D<float4> gCellsOut : register(u0);

static const uint kTileW = 26; // 2 cells x 8 px + 8 px window overhang + 2 gradient taps
groupshared float4 sRedA[64];
groupshared float4 sRedB[64];
groupshared float sTile[kTileW * kTileW];
groupshared float2 sV[4];    // current estimate per cell, broadcast
groupshared float4 sPred[4]; // vPred.xy, age, 0

float L(Texture2D<float4> t, float2 pos) { return t.SampleLevel(gLinear, pos * gIn.zw, 0).a; }
float LPrev(float2 pos) { return (gU1.w & 8u) != 0 ? L(gWorkPrev, pos) : gRef.SampleLevel(gLinear, pos * gIn.zw, 0); }

// Sum over the 16 lanes of each cell (segments of sRedA / sRedB); result in [cell * 16], visible after the call.
void Reduce16(uint lane, uint gi, bool both)
{
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 8; s > 0; s >>= 1) {
        if (lane < s) {
            sRedA[gi] += sRedA[gi + s];
            if (both) sRedB[gi] += sRedB[gi + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint q = gi >> 4, lane = gi & 15u;
    const int2 c = int2(gid.xy) * 2 + int2(q & 1u, q >> 1);
    const bool inGrid = all(c < int2(gCell.xy));
    const int2 cc = min(c, int2(gCell.xy) - 1); // out-of-grid lanes compute a duplicate and write nothing
    const float4 f = gFlow.Load(int4(cc, 1, 0));
    // w < 0 marks "no valid vector" (no history, or a scene cut: the next frame must not predict from it).
    // Both conditions are uniform over the dispatch.
    if (gU0.x == 0) {
        if (lane == 0 && inGrid) gCellsOut[c] = float4(0, 0, 0, -1);
        return;
    }
    if (gU1.y != 0 && gStats[gU1.x] != 0) {
        if (lane == 0 && inGrid) gCellsOut[c] = float4(f.xy, 0, -1);
        return;
    }
    const float cellPx = gCell.z;
    const float2 xc = (float2(cc) + 0.5) * cellPx;
    const bool havePred = gU2.x != 0 && (gU1.w & 16u) == 0; // development flag 16: no velocity filter
    if (lane == 0) {
        // Velocity prediction: the refined vector of the same content one frame earlier (constant-velocity model).
        float2 vPred = f.xy;
        float age = 0.0;
        if (havePred) {
            const float4 pc = gPrevCells.SampleLevel(gPoint, (xc + f.xy) * gCell.w / gCell.xy, 0);
            const float4 pl = gPrevCells.SampleLevel(gLinear, (xc + f.xy) * gCell.w / gCell.xy, 0);
            vPred = pl.xy;
            age = max(pc.w, 0.0);
            if (pc.w < 0.0 || pl.w < 0.0 || length(vPred - f.xy) > 2.0) {
                vPred = f.xy;
                age = 0.0;
            }
        }
        sPred[q] = float4(vPred, age, 0);
        sV[q] = vPred;
    }
    const bool tiled = cellPx == 8.0;
    // Tile texel (i, j) = frame texel gid * 16 - 5 + (i, j); cell q's window sample (sx, sy) is tile texel
    // ((q & 1) * 8 + sx + 1, (q >> 1) * 8 + sy + 1).
    if (tiled) {
        const int2 t0 = int2(gid.xy) * 16 - 5;
        const int2 hi = int2(gIn.xy) - 1;
        for (uint k0 = gi; k0 < kTileW * kTileW; k0 += 64)
            sTile[k0] = gWorkCurr.Load(int3(clamp(t0 + int2(k0 % kTileW, k0 / kTileW), int2(0, 0), hi), 0)).a;
    }
    GroupMemoryBarrierWithGroupSync();
    const float step = cellPx / 8.0;
    const float invS2 = 1.0 / (0.25 * cellPx * cellPx * 2.0);
    const uint tx = (q & 1u) * 8 + lane + 1, tyBase = (q >> 1) * 8 + 1;
    // Current-frame terms of sample (lane, m): weight, luma and gradients.
    float4 accA = 0, accB = 0;
    [loop] for (uint m = 0; m < 16; ++m) {
        const float2 o = (float2(lane, m) - 7.5) * step;
        const float w = exp(-dot(o, o) * invS2);
        float gx, gy;
        if (tiled) {
            const uint ti = (tyBase + m) * kTileW + tx;
            gx = 0.5 * (sTile[ti + 1] - sTile[ti - 1]);
            gy = 0.5 * (sTile[ti + kTileW] - sTile[ti - kTileW]);
        } else {
            const float2 p = xc + o;
            gx = 0.5 * (L(gWorkCurr, p + float2(1, 0)) - L(gWorkCurr, p - float2(1, 0)));
            gy = 0.5 * (L(gWorkCurr, p + float2(0, 1)) - L(gWorkCurr, p - float2(0, 1)));
        }
        accA += float4(w, w * gx, w * gy, 0);
        accB += float4(w * gx * gx, w * gx * gy, w * gy * gy, 0);
    }
    // v-independent sums (sw, sgx, sgy) and (sgxx, sgxy, sgyy) in one reduction.
    sRedA[gi] = accA;
    sRedB[gi] = accB;
    Reduce16(lane, gi, true);
    const uint r0 = q * 16;
    const float sw = sRedA[r0].x;
    const float iw = 1.0 / max(sw, 1e-6);
    const float mgx = sRedA[r0].y * iw, mgy = sRedA[r0].z * iw;
    const float lam = 2e-5 * sw;
    const float a11 = sRedB[r0].x - sw * mgx * mgx + lam;
    const float a12 = sRedB[r0].y - sw * mgx * mgy;
    const float a22 = sRedB[r0].z - sw * mgy * mgy + lam;
    const float det = a11 * a22 - a12 * a12;
    const uint iters = gU2.w;
    for (uint it = 0; it < iters; ++it) {
        GroupMemoryBarrierWithGroupSync();
        const float2 v = sV[q];
        float4 acc = 0;
        [loop] for (uint m2 = 0; m2 < 16; ++m2) {
            const float2 o = (float2(lane, m2) - 7.5) * step;
            const float w = exp(-dot(o, o) * invS2);
            const float2 p = xc + o;
            float lc, gx, gy;
            if (tiled) {
                const uint ti = (tyBase + m2) * kTileW + tx;
                lc = sTile[ti];
                gx = 0.5 * (sTile[ti + 1] - sTile[ti - 1]);
                gy = 0.5 * (sTile[ti + kTileW] - sTile[ti - kTileW]);
            } else {
                lc = L(gWorkCurr, p);
                gx = 0.5 * (L(gWorkCurr, p + float2(1, 0)) - L(gWorkCurr, p - float2(1, 0)));
                gy = 0.5 * (L(gWorkCurr, p + float2(0, 1)) - L(gWorkCurr, p - float2(0, 1)));
            }
            const float d = LPrev(p + v) - lc;
            acc += float4(w * d, w * gx * d, w * gy * d, 0);
        }
        sRedA[gi] = acc;
        Reduce16(lane, gi, false);
        if (lane == 0) {
            const float md = sRedA[r0].x * iw;
            const float b1 = sRedA[r0].y - sw * mgx * md;
            const float b2 = sRedA[r0].z - sw * mgy * md;
            float2 dv = -float2(a22 * b1 - a12 * b2, a11 * b2 - a12 * b1) / max(det, 1e-12);
            const float len = length(dv);
            if (len > 1.0) dv /= len;
            sV[q] = v + dv;
        }
    }
    if (lane != 0 || !inGrid) return;
    float2 v = sV[q]; // last written by this thread
    const float2 vPred = sPred[q].xy;
    float age = sPred[q].z;
    // Measurements of aliased content jump between frames (thin features move in whole-pixel steps on a fixed sample
    // grid) although their long-run average is the true velocity; the prediction is blended with the measurement,
    // fully trusting the measurement when they disagree by more than gP4.z frame px (real change of motion).
    if (havePred && age > 0.0) {
        const float dev = smoothstep(gP4.y, gP4.z, length(v - vPred));
        const float k = lerp(max(1.0 / (age + 1.0), gP4.x), 1.0, dev);
        v = lerp(vPred, v, k);
        age = dev > 0.5 ? 0.0 : age;
    }
    gCellsOut[c] = float4(v, f.z, min(age + 1.0, 64.0));
}
