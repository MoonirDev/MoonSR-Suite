// Moon ALSR pass 5 (output resolution): measurement update of the history. Owner: upscaling-engineer.
// MRT0 (RGBA16F, the history): rgb = clamp(Hw + g * R_up, 3x3 source hull +- margin), a = min(count + 1, maxCount).
// MRT1 (RGBA8, gamma copy): the same colour written through an _SRGB render-target view when the working space is
//      linear (the output merger does the sRGB encoding; UNORM view otherwise), a = count / maxCount. alsr_final and
//      alsr_refluma read it through a UNORM view, i.e. as gamma-encoded values, without any pow() per tap (Stage 3
//      cost cut: the per-tap FromWork() made those two passes transcendental-bound).
// Variant ALSR_UPDATE_DIRECT (g_ps_alsr_update_direct; Render uses it when sharpness is 0 and no debug view is on, where
// alsr_final would be an exact copy): MRT1 = the caller's destination and MRT2 = the LastOutput copy, both
// FromWork(colour), and no gamma copy - alsr_final is skipped (Stage 3 cost cut, one output-resolution pass).
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
Texture2D<float4> gResidual : register(t1); // source res
Texture2D<float4> gWork : register(t2);     // current frame, working space, source res
Texture2D<float4> gMotion : register(t3);   // per-pixel motion (alsr_motion, source res)
Texture2D<float4> gWorkPrev : register(t4); // previous frame, working space, source res (unbound without history)

struct PsOut
{
    float4 hist : SV_Target0;
#ifdef ALSR_UPDATE_DIRECT
    float4 dst : SV_Target1;
    float4 copy : SV_Target2;
#else
    float4 gammaCopy : SV_Target1;
#endif
};

PsOut main(VsOut i)
{
    const float4 hw = gHw.Load(int3(int2(i.pos.xy), 0));
    const float3 r = SampleCR(gResidual, i.pos.xy * gScale.zw, gIn.zw, true).rgb;
    const float count = hw.a;
    const float2 x = i.pos.xy * gScale.zw;
    const int2 xi = clamp(int2(x), int2(0, 0), int2(gIn.xy) - 1);
    float g = max(max(1.0 / (count + 1.0), gP0.y), smoothstep(gP2.x, gP2.y, abs(LumaOf(r))));
    if ((gU1.w & 1024u) == 0) {
        if (count <= 0.0) {
            g = 0.0;
        } else if (all(gMotion.Load(int3(xi, 0)).xy == 0.0)) {
            const float dc = abs(LumaOf(gWork.Load(int3(xi, 0)).rgb - gWorkPrev.Load(int3(xi, 0)).rgb));
            g = smoothstep(gP2.x, gP2.y, dc);
        }
    }
    float3 h = hw.rgb + g * r;
    // Anti-ringing: the back-projection can overshoot at hard edges; clamp to the 3x3 source min / max (RGB) expanded
    // by gP4.w x its range, the same margin the reprojection clip allows.
    float3 lo = 1e9, hi = -1e9;
    [unroll] for (int k = 0; k < 9; ++k) {
        const float3 c = gWork.Load(int3(clamp(xi + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), int2(gIn.xy) - 1), 0)).rgb;
        lo = min(lo, c);
        hi = max(hi, c);
    }
    const float3 mr = gP4.w * (hi - lo);
    if ((gU1.w & 256u) == 0) h = clamp(h, lo - mr, hi + mr); // development flag 256: no anti-ringing clamp
    h = saturate(h);
    PsOut o;
    o.hist = float4(h, min(count + 1.0, gP0.w));
#ifdef ALSR_UPDATE_DIRECT
    o.dst = float4(FromWork(h), 1.0);
    o.copy = o.dst;
#else
    o.gammaCopy = float4(h, saturate(o.hist.a / max(gP0.w, 1.0)));
#endif
    return o;
}
