// Interpolate stage 3 (per generated frame, half frame resolution): robust motion hypothesis per 2x2 pixel block.
//
// Replaces the former per-pixel winner-take-all candidate choice in the interpolation shader, which compared single
// pixels: on aliased or flat content the choice flipped from pixel to pixel (salt-and-pepper motion, "crunchy" edges),
// in occlusion zones a wrong vector often matched by chance (speckles), and the cell field's 8-px blocks showed as
// seams under rotation / zoom. Here, per selection texel g (frame position p = 2 g + 1, the corner shared by its 2x2
// pixels):
//  1. Candidates: the bilinearly upsampled time-t field at p (smooth motion; wins wherever the field is smooth) and
//     the 2x2 (gU0.y = 4) or 3x3 (9) nearest time-t cells (motion boundaries). Near-duplicates are skipped.
//  2. Patch error: mean |L0(p + o - t m) - L1(p + o + (1 - t) m) - mu| over a 3x3 patch with
//     2-px spacing on the flow-resolution luma, mu = coarse local mean-brightness difference (gMean*).
//  3. Visibility per side from the pair's own fields, bilinear over the 4 nearest cells (no block steps):
//     vis0 = sum_i w_i exp(-0.5 (|F01_i(x0) - m| / tau)^2), likewise vis1 with F10 = -m; times (1 - HUD mask) at the
//     fetch position for moving content (gU1.x = HUD protection on). A candidate visible on one
//     side only cannot be checked photometrically; its error is replaced by gP1.w (occlusion cost), so in a
//     disocclusion the geometrically consistent one-sided vector wins over vectors that match by chance.
//     score = both * err + (1 - both) * occCost + 8 (1 - cellConf) [- 1 for the smooth candidate]  (luma levels)
//  4. Output for the winner: blend share of frame 1, beta = w1 / (w0 + w1), w0 = (1-t)(vis0+0.01), w1 = t(vis1+0.01);
//     confidence = cellConf * lerp(0.85 max(vis0, vis1), agree, both), agree = exp(-0.5 (err / sigma)^2). A one-sided
//     vector whose other side is covered by a CONFIDENT different motion is an explained occlusion: its confidence is
//     raised towards 0.75 max(vis) (fwd-bwd consistency is low for every occluded cell by construction, which used to
//     send disocclusions to the ghosting blend fallback).
//
//   gSrcSize frame size   gDstSize cell grid   gFieldSize selection grid   gAuxSize mean-luma level size
//   gP0 = (t, cell px, 1 / cell px, -)   gP1 = (agree sigma (luma levels), tau0 frame px, tauK, occlusion cost)
//   gU0 = (-, candidate cells 4|9, -, -)   gU1.x = HUD protection
// Output u0: float4(m.xy 0->1 motion in frame px, beta, confidence); u1: float2(both, agree).
#include "framegen/fg_common.hlsli"

Texture2D<float4> gTField : register(t0);
Texture2DArray<float4> gField : register(t1);
Texture2D<float> gLum0 : register(t2);
Texture2D<float> gLum1 : register(t3);
Texture2D<float> gMean0 : register(t4);
Texture2D<float> gMean1 : register(t5);
Texture2D<float> gHud : register(t6);
SamplerState gLinear : register(s0);
RWTexture2D<float4> gOut : register(u0);
RWTexture2D<float2> gOutAux : register(u1);

// Bilinear-weighted consistency of field `slice` at frame position x with the expected vector e.
// Returns (visibility, confidence-weighted disagreement = how confidently something ELSE is there).
float2 Visibility(float2 x, uint slice, float2 e, float tau, float2 frame)
{
    if (any(x < 0.0) || any(x > frame)) return float2(0.0, 0.0);
    const float2 cf = x * gP0.z - 0.5;
    const float2 c0 = floor(cf);
    const float2 f = cf - c0;
    float vis = 0.0, other = 0.0;
    [unroll] for (int k = 0; k < 4; ++k) {
        const int2 o = int2(k & 1, k >> 1);
        const float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
        const float4 F = gField.Load(int4(ClampI(int2(c0) + o, gDstSize), slice, 0));
        const float d = length(F.xy - e) / tau;
        const float v = exp(-0.5 * d * d);
        vis += w * v;
        other += w * (1.0 - v) * saturate(F.z);
    }
    return float2(vis, other);
}

float PatchError(float2 p, float2 m, float t, float2 invFrame)
{
    const float2 x0 = p - t * m;
    const float2 x1 = p + (1.0 - t) * m;
    const float mu = gMean0.SampleLevel(gLinear, x0 * invFrame, 0) - gMean1.SampleLevel(gLinear, x1 * invFrame, 0);
    float e = 0.0;
    [unroll] for (uint j = 0; j < 9; ++j) {
        const float2 o = float2(float(j % 3) - 1.0, float(j / 3) - 1.0) * 2.0;
        e += abs(gLum0.SampleLevel(gLinear, (x0 + o) * invFrame, 0) - gLum1.SampleLevel(gLinear, (x1 + o) * invFrame, 0) - mu);
    }
    return e * (1.0 / 9.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 g = int2(id.xy);
    if (any(g >= gFieldSize)) return;
    const float t = gP0.x;
    const float2 frame = float2(gSrcSize);
    const float2 invFrame = 1.0 / frame;
    const float2 p = min(float2(g) * 2.0 + 1.0, frame - 0.5);

    // Smooth candidate: bilinear time-t field (valid cells only).
    const float2 cf = p * gP0.z - 0.5;
    const int2 cb = int2(floor(cf));
    const float2 f = cf - float2(cb);
    float3 smooth = 0.0;
    float sw = 0.0;
    [unroll] for (int q = 0; q < 4; ++q) {
        const int2 o = int2(q & 1, q >> 1);
        const float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
        const float4 tf = gTField.Load(int3(ClampI(cb + o, gDstSize), 0));
        smooth += tf.w * w * tf.xyz;
        sw += tf.w * w;
    }
    smooth = sw > 1e-4 ? smooth / sw : float3(0, 0, 0);

    const int span = gU0.y >= 9 ? 3 : 2;
    const int2 base = span == 3 ? int2(round(cf)) - 1 : cb;
    const float tau0 = gP1.y, tauK = gP1.z, occCost = gP1.w;

    float bestScore = 1e9;
    float2 bestM = smooth.xy;
    float bestConf = 0.0, bestErr = 0.0, bestV0 = 0.0, bestV1 = 0.0, bestO0 = 0.0, bestO1 = 0.0;
    // Near-duplicates are skipped by comparing with the smooth candidate and the last evaluated one (registers only; an
    // indexable list of all evaluated vectors is placed in memory by the compiler).
    float2 last = float2(1e9, 1e9);
    [loop] for (int k = -1; k < span * span; ++k) {
        float2 m;
        float cconf;
        if (k < 0) {
            m = smooth.xy;
            cconf = smooth.z;
        } else {
            const float4 tf = gTField.Load(int3(ClampI(base + Unflatten(k, span), gDstSize), 0));
            if (tf.w == 0.0) continue;
            m = tf.xy;
            cconf = tf.z;
        }
        if (k >= 0 && (all(abs(m - smooth.xy) < 0.05) || all(abs(m - last) < 0.05))) continue;
        last = m;

        const float tau = tau0 + tauK * length(m);
        float2 v0 = Visibility(p - t * m, 0, m, tau, frame);
        float2 v1 = Visibility(p + (1.0 - t) * m, 1, -m, tau, frame);
        if (gU1.x != 0) {
            // Moving content is never read from protected overlay pixels (the overlay covers it there).
            const float mv = saturate(length(m) - 0.5);
            const float h0 = mv * gHud.SampleLevel(gLinear, (p - t * m) * invFrame, 0);
            const float h1 = mv * gHud.SampleLevel(gLinear, (p + (1.0 - t) * m) * invFrame, 0);
            v0 = float2(v0.x * (1.0 - h0), max(v0.y, h0));
            v1 = float2(v1.x * (1.0 - h1), max(v1.y, h1));
        }
        const float both = v0.x * v1.x;
        const float err = PatchError(p, m, t, invFrame);
        const float score = both * err + (1.0 - both) * occCost + 8.0 * (1.0 - cconf) - (k < 0 ? 0.5 : 0.0);
        if (score < bestScore) {
            bestScore = score;
            bestM = m;
            bestConf = cconf;
            bestErr = err;
            bestV0 = v0.x;
            bestV1 = v1.x;
            bestO0 = v0.y;
            bestO1 = v1.y;
        }
    }

    const float w0 = (1.0 - t) * (bestV0 + 0.01);
    const float w1 = t * (bestV1 + 0.01);
    const float both = bestV0 * bestV1;
    const float agree = exp(-0.5 * (bestErr / gP1.x) * (bestErr / gP1.x));
    float conf = bestConf * lerp(0.85 * max(bestV0, bestV1), agree, both);
    // Explained occlusion: visible on one side, the other side confidently covered by a different motion.
    const float explained = saturate(bestV1 * bestO0 + bestV0 * bestO1) * (1.0 - both);
    conf = lerp(conf, max(conf, 0.75 * max(bestV0, bestV1)), explained);
    gOut[g] = float4(bestM, w1 / (w0 + w1), conf);
    gOutAux[g] = float2(both, agree);
}
