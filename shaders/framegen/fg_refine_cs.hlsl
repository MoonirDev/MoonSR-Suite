// Sub-block refinement at the finest level, both directions. One thread per 4x4 cell of level 0.
//
// Candidates: the 3x3 regularised block vectors around the cell's parent block (gU0.x = 9), or the parent plus the
// three neighbours on the cell's side (gU0.x = 4). Each is scored by the rms zero-mean difference over a 6x6 window
// centred on the cell (bilinear), plus mu2 * |v - v_parent|_1. (An integer-rounded candidate was tried and removed:
// integer offsets avoid the bilinear blur of the reference and win unfairly, biasing sub-pixel motion to 0.) The
// winner then gets gU0.y sub-pixel steps: the costs at +-h along x and y are evaluated, SubPixel() gives the step per
// axis, and the step is kept only if it lowers the cost; h = 0.5, 0.25. Pixels are weighted by (1 - previous HUD mask)
// as in the search (uniform if the window is mostly overlay).
//   gSrcSize level-0 size   gDstSize cell grid size   gFieldSize block grid size of the parent level
//   gP0.x mu2
//   gU0.x candidates (4 or 9)   gU0.y sub-pixel steps (0..2)   gU0.z log2(cells per parent block): 1 = parent blocks
//   of level 0, 2 = of level 1 (the level-0 search is skipped; vectors are scaled x2)   gU1.x use HUD weights
// Output (slice = direction): float4(vector in level-0 px, rms cost, source std dev, 0).
#include "framegen/fg_common.hlsli"

Texture2D<float> gLumPrev : register(t0);
Texture2D<float> gLumCurr : register(t1);
Texture2DArray<float4> gField : register(t2);
Texture2D<float> gHud : register(t3);
SamplerState gLinear : register(s0);
RWTexture2DArray<float4> gOut : register(u0);

#define WIN 6
#define WIN2 (WIN * WIN)

// Per-thread source window and weights live in groupshared memory and the SSD runs as a real loop: fully unrolled
// register arrays made this shader ~9500 instruction slots (7 call sites x 36 taps x 2 directions).
groupshared float sSrc[64 * WIN2];
groupshared float sWt[64 * WIN2];

// Weighted zero-mean SSD per unit weight (a mean squared difference). `base` = this thread's offset in sSrc / sWt.
float Ssd(Texture2D<float> ref, uint base, float wsum, bool refW, int2 org, float2 v)
{
    const float2 invSize = 1.0 / float2(gSrcSize);
    float sd = 0.0, sd2 = 0.0, sw = 0.0;
    [loop] for (int y = 0; y < WIN; ++y) {
        [unroll] for (int x = 0; x < WIN; ++x) {
            const uint i = base + uint(y * WIN + x);
            const float2 uv = (float2(org + int2(x, y)) + 0.5 + v) * invSize;
            float w = sWt[i];
            if (refW) w *= 1.0 - gHud.SampleLevel(gLinear, uv, 0);
            const float d = sSrc[i] - ref.SampleLevel(gLinear, uv, 0);
            sd += w * d;
            sd2 += w * d * d;
            sw += w;
        }
    }
    if (sw < 0.25 * wsum) return 1e8;
    return max(sd2 - sd * sd / sw, 0.0) / sw;
}

float4 Refine(Texture2D<float> srcTex, Texture2D<float> ref, int2 cell, uint dir, uint base)
{
    const int2 org = cell * kCell - 1;
    float sum = 0.0, sum2 = 0.0, wsum = 0.0;
    [loop] for (int i = 0; i < WIN2; ++i) {
        const int2 q = org + Unflatten(i, WIN);
        const float s = srcTex.Load(int3(ClampI(q, gSrcSize), 0));
        const float w = gU1.x != 0 ? 1.0 - gHud.SampleLevel(gLinear, (float2(q) + 0.5) / float2(gSrcSize), 0) : 1.0;
        sSrc[base + uint(i)] = s;
        sWt[base + uint(i)] = w;
        sum += s;
        sum2 += s * s;
        wsum += w;
    }
    const float n = float(WIN2);
    const float stdev = sqrt(max(sum2 / n - (sum / n) * (sum / n), 0.0));
    bool refW = gU1.x != 0;
    // Most of the frame is far from any overlay: if the source window has full weight and the four corners of the
    // window displaced by the parent vector see no mask either, the per-sample reference weights are skipped.
    // (Approximation: an overlay smaller than the window between the corners is missed; the search and
    // regularisation still applied full weights on the way down.)
    if (gU1.y != 0 && refW && wsum > float(WIN2) - 0.01) {
        const float2 vP0 = gField.Load(int4(ClampI(int2(uint2(cell) >> gU0.z), gFieldSize), dir, 0)).xy * float(1u << (gU0.z - 1u));
        const float2 o0 = (float2(org) + vP0) / float2(gSrcSize), o1 = (float2(org + WIN) + vP0) / float2(gSrcSize);
        const float h = gHud.SampleLevel(gLinear, o0, 0) + gHud.SampleLevel(gLinear, float2(o1.x, o0.y), 0) +
                        gHud.SampleLevel(gLinear, float2(o0.x, o1.y), 0) + gHud.SampleLevel(gLinear, o1, 0);
        refW = h > 0.0;
    }
    if (wsum < 9.0) {
        [loop] for (int k1 = 0; k1 < WIN2; ++k1) sWt[base + uint(k1)] = 1.0;
        wsum = n;
        refW = false;
    }

    const int2 parent = int2(uint2(cell) >> gU0.z);
    const float vs = float(1u << (gU0.z - 1u)); // parent level px -> level-0 px
    const float2 vParent = gField.Load(int4(ClampI(parent, gFieldSize), dir, 0)).xy * vs;
    const int2 inBlock = int2(uint2(cell) & ((1u << gU0.z) - 1u)) * 2 + 1 - int(1u << gU0.z); // sign of the cell's side
    const int2 side = int2(inBlock.x > 0 ? 1 : -1, inBlock.y > 0 ? 1 : -1);

    float2 best = vParent;
    float c0 = Ssd(ref, base, wsum, refW, org, vParent);
    float bestCost = sqrt(c0);
    const uint nc = gU0.x == 9 ? 8u : 3u;
    [loop] for (uint k = 0; k < nc; ++k) {
        int2 off;
        if (gU0.x == 9) {
            const uint kk = k < 4 ? k : k + 1; // skip the centre
            off = Unflatten(int(kk), 3) - 1;
        } else {
            off = k == 0 ? int2(side.x, 0) : (k == 1 ? int2(0, side.y) : side);
        }
        const float2 v = gField.Load(int4(ClampI(parent + off, gFieldSize), dir, 0)).xy * vs;
        // The parent was already scored with zero regularisation penalty.
        if (all(v == vParent)) continue;
        const float ssd = Ssd(ref, base, wsum, refW, org, v);
        const float c = sqrt(ssd) + gP0.x * dot(abs(v - vParent), 1.0);
        if (c < bestCost) {
            bestCost = c;
            best = v;
            c0 = ssd;
        }
    }

    float h = 0.5;
    [loop] for (uint s = 0; s < gU0.y; ++s) {
        // An exact match produces a zero subpixel offset at every remaining step.
        if (c0 == 0.0) break;
        const float cl = Ssd(ref, base, wsum, refW, org, best - float2(h, 0));
        const float cr = Ssd(ref, base, wsum, refW, org, best + float2(h, 0));
        const float cu = Ssd(ref, base, wsum, refW, org, best - float2(0, h));
        const float cd = Ssd(ref, base, wsum, refW, org, best + float2(0, h));
        const float r0 = sqrt(c0);
        const float2 delta = h * float2(SubPixel(r0, sqrt(cl), sqrt(cr)), SubPixel(r0, sqrt(cu), sqrt(cd)));
        const float cn = Ssd(ref, base, wsum, refW, org, best + delta);
        if (cn <= c0) {
            best += delta;
            c0 = cn;
        }
        h *= 0.5;
    }
    return float4(best, sqrt(c0), stdev);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    const int2 cell = int2(id.xy);
    const uint dir = id.z;
    if (any(cell >= gDstSize)) return;
    const uint base = gi * WIN2;
    float4 r;
    [branch] if (dir == 0) r = Refine(gLumPrev, gLumCurr, cell, dir, base);
    else r = Refine(gLumCurr, gLumPrev, cell, dir, base);
    gOut[uint3(cell, dir)] = r;
}
