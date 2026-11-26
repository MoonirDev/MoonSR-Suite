// Moon ALSR pass 5 (output resolution): measurement update of the history. Owner: upscaling-engineer.
// MRT0 (RGBA16F, the history): rgb = clamp(Hw + g * R_up, 3x3 source hull +- margin), a = min(count + 1, maxCount).
// MRT1 (RGBA8, gamma copy): the same colour written through an _SRGB render-target view when the working space is
//      linear (the output merger does the sRGB encoding; UNORM view otherwise), a = count / maxCount. alsr_final and
//      alsr_refluma read it through a UNORM view, i.e. as gamma-encoded values, without any pow() per tap (Stage 3
//      cost cut: the per-tap FromWork() made those two passes transcendental-bound).
// Variant ALSR_UPDATE_DIRECT (g_ps_alsr_update_direct; Render uses it when sharpness is 0 and no debug view is on, where
// alsr_final would be an exact copy): MRT1 = FromWork(colour) in the caller's destination,
// with no gamma copy - alsr_final is skipped (Stage 3 cost cut, one output-resolution pass).
//   R_up : the source-resolution residual (pass 4) interpolated to this output pixel (Catmull-Rom, 2x2 clamped).
//   g    : gain.
//          - A fresh pixel (count 0: no history, scene cut, reset, rejected) takes g = 0: the history restarts from
//            exactly the single-frame reconstruction that pass 3 put into Hw (contract: "restarts from the best
//            single-frame reconstruction").
//          - A pixel whose motion is exactly zero (static content, unchanged input or static overlay: alsr_motion writes
//            v = 0 for those) receives no new sample phase, so the residual carries no new information; there g only
//            follows an in-place change of the input (smoothstep(gP2.x, gP2.y, |luma(C - C_prev)|) at the source pixel).
//          - Otherwise max(1 / (count + 1), gMin), i.e. a running average of the residual corrections up to the floor
//            gMin, raised towards 1 where the residual is large (gP2.x .. gP2.y, luma of |R_up|): a large residual means
//            the history mispredicts the frame, and following the current frame there removes lag and ghosting.
//          Why the first two rules (measured, tbench static 1.5x aliased, RTX 5060): without them the back-projection
//          kept integrating the mismatch between its area sampling model and point-sampled input on content that never
//          moves, a slow deconvolution that never converged (24.6 dB after 32 frames and still falling, vs 27.97 dB for
//          the single-frame reconstruction it started from; dark ringing on every hard edge and around HUD glyphs).
//          Development flag 1024 restores the previous behaviour (g = 1 at count 0, no zero-motion rule).
// This is a recursive form of iterative back-projection (Irani & Peleg 1991; Farsiu et al. 2004): low frequencies
// follow the current frame, detail that the current samples cannot resolve on their own is carried by the history,
// and every new sub-pixel phase that scene motion brings adds the information it contains.
#include "upscale/alsr_common.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> gHw : register(t0);       // output res
#if defined(ALSR_FUSED_ACCUMULATE) || defined(ALSR_POINT_EVIDENCE)
Texture2D<float2> gLayer : register(t1);   // source res
#else
Texture2D<float4> gResidual : register(t1); // source res
#endif
Texture2D<float4> gWork : register(t2);     // current frame, working space, source res
Texture2D<float4> gMotion : register(t3);   // per-pixel motion (alsr_motion, source res)
Texture2D<float4> gWorkPrev : register(t4); // previous frame, working space, source res (unbound without history)

#ifdef ALSR_GUARDED_POINT
Texture2D<float2> gLayer : register(t5);
Texture2D<float4> gSampling : register(t6);
#endif

#ifdef ALSR_ROBUST_HISTORY
Texture2D<float4> gCurrentState : register(t5);
#endif

#ifdef ALSR_FUSED_ACCUMULATE
#include "upscale/alsr_residual.hlsli"

float3 FusedResidual(float2 p)
{
#ifdef ALSR_FUSED_CUBIC
    const float2 base = floor(p - 0.5);
    const int2 index = int2(base);
    const int2 hi = int2(gIn.xy) - 1;
    float2 w0, w1, w2, w3;
    CubicWeights(p - 0.5 - base, -0.5, w0, w1, w2, w3);
    const float2 weights[4] = {w0, w1, w2, w3};
    float3 sum = 0;
    float3 lo = 1e9, upper = -1e9;
    [unroll] for (int y = 0; y < 4; ++y) {
        float3 row = 0;
        [unroll] for (int x = 0; x < 4; ++x) {
            const int2 sampleIndex = clamp(index + int2(x - 1, y - 1), 0, hi);
            // The separate pass stores its residual as RGBA16F before the cubic fetch.
            const float3 v = f16tof32(f32tof16(MeasurementResidual(sampleIndex)));
            row += v * weights[x].x;
            if (x >= 1 && x <= 2 && y >= 1 && y <= 2) {
                lo = min(lo, v);
                upper = max(upper, v);
            }
        }
        sum += row * weights[y].y;
    }
    return clamp(sum, lo, upper);
#else
    // Four nearby residuals reuse the existing area measurement model. Bilinear interpolation
    // keeps only non-negative weights and avoids the sixteen evaluations of a cubic kernel.
    const float2 base = floor(p - 0.5);
    const float2 phase = p - 0.5 - base;
    const int2 index = int2(base);
    const int2 hi = int2(gIn.xy) - 1;
    const float3 a = MeasurementResidual(clamp(index, 0, hi));
    const float3 b = MeasurementResidual(clamp(index + int2(1, 0), 0, hi));
    const float3 c = MeasurementResidual(clamp(index + int2(0, 1), 0, hi));
    const float3 d = MeasurementResidual(clamp(index + 1, 0, hi));
    return lerp(lerp(a, b, phase.x), lerp(c, d, phase.x), phase.y);
#endif
}
#endif

#ifdef ALSR_GATED_UPDATE
float3 FusedResidual(float2 p)
{
    return SampleCR(gResidual, p, gIn.zw, true).rgb;
}
#endif

struct PsOut
{
    float4 hist : SV_Target0;
#ifdef ALSR_UPDATE_DIRECT
    float4 dst : SV_Target1;
#else
    float4 gammaCopy : SV_Target1;
#endif
};

#if defined(ALSR_POINT_EVIDENCE) || defined(ALSR_GUARDED_POINT)
#include "upscale/alsr_point.hlsli"
#endif

PsOut main(VsOut i)
{
    const float4 hw = gHw.Load(int3(int2(i.pos.xy), 0));
#ifdef ALSR_POINT_EVIDENCE
    const float4 evidence = AccumulatePoint(hw, i.pos.xy * gScale.zw);
    const float3 h = evidence.rgb;
#else
#if !defined(ALSR_FUSED_ACCUMULATE) && !defined(ALSR_GATED_UPDATE)
    const float3 r = SampleCR(gResidual, i.pos.xy * gScale.zw, gIn.zw, true).rgb;
#endif
    const float count = hw.a;
    const float2 x = i.pos.xy * gScale.zw;
    const int2 xi = clamp(int2(x), int2(0, 0), int2(gIn.xy) - 1);
#if defined(ALSR_FUSED_ACCUMULATE) || defined(ALSR_GATED_UPDATE)
    float3 r = 0;
    float g = 0;
    const bool forceGain = (gU1.w & 1024u) != 0;
    [branch] if (forceGain || count > 0.0) {
        if (forceGain || any(gMotion.Load(int3(xi, 0)).xy != 0.0)) {
            r = FusedResidual(x);
            g = max(max(1.0 / (count + 1.0), gP0.y), smoothstep(gP2.x, gP2.y, abs(LumaOf(r))));
        } else {
            const float dc = abs(LumaOf(gWork.Load(int3(xi, 0)).rgb - gWorkPrev.Load(int3(xi, 0)).rgb));
            g = smoothstep(gP2.x, gP2.y, dc);
            [branch] if (g > 0.0) r = FusedResidual(x);
        }
    }
#else
    float g = max(max(1.0 / (count + 1.0), gP0.y), smoothstep(gP2.x, gP2.y, abs(LumaOf(r))));
    if ((gU1.w & 1024u) == 0) {
        if (count <= 0.0) {
            g = 0.0;
        } else if (all(gMotion.Load(int3(xi, 0)).xy == 0.0)) {
            const float dc = abs(LumaOf(gWork.Load(int3(xi, 0)).rgb - gWorkPrev.Load(int3(xi, 0)).rgb));
            g = smoothstep(gP2.x, gP2.y, dc);
        }
    }
#endif
    float3 h = hw.rgb + g * r;
    // Anti-ringing: the back-projection can overshoot at hard edges; clamp to the 3x3 source min / max (RGB) expanded
    // by gP4.w x its range, the same margin the reprojection clip allows.
    float3 lo = 1e9, hi = -1e9;
    [unroll] for (int k = 0; k < 9; ++k) {
        const float3 c = gWork.Load(int3(clamp(xi + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), int2(gIn.xy) - 1), 0)).rgb;
        lo = min(lo, c);
        hi = max(hi, c);
    }
#ifdef ALSR_ROBUST_HISTORY
    const float lock = gCurrentState.Load(int3(int2(i.pos.xy), 0)).x;
    const float3 mr = gP4.w * (hi - lo) + 0.20 * lock;
#else
    const float3 mr = gP4.w * (hi - lo);
#endif
    if ((gU1.w & 256u) == 0) h = clamp(h, lo - mr, hi + mr); // development flag 256: no anti-ringing clamp
    h = saturate(h);
#endif
#ifdef ALSR_GUARDED_POINT
    const float4 area = float4(h, min(count + 1.0, gP0.w));
    const bool usePoint = (gU1.w & (1u << 18)) == 0 || gSampling.Load(int3(0, 0, 0)).x == 2.0;
    const float4 combined = BlendPoint(area, hw, x, xi, usePoint);
    h = combined.rgb;
#endif
    PsOut o;
#ifdef ALSR_POINT_EVIDENCE
    o.hist = evidence;
#elif defined(ALSR_GUARDED_POINT)
    o.hist = combined;
#else
    o.hist = float4(h, min(count + 1.0, gP0.w));
#endif
#ifdef ALSR_UPDATE_DIRECT
    o.dst = float4(FromWork(h), 1.0);
#else
    o.gammaCopy = float4(h, saturate(o.hist.a / max(gP0.w, 1.0)));
#endif
    return o;
}
