// Interpolate stage 4: synthesise the frame at phase t at full resolution. Drawn with common/fullscreen_vs.hlsl over
// a viewport of exactly the frame size, into two targets: the colour and an anti-aliasing guide for fg_aa_ps.
//
// Per pixel y, from the 2x2 nearest selection texels S_k (fg_pixsel_cs, half resolution, bilinear weights b_k):
//  1. Smooth region (all four motions within 0.5 px of their bilinear mean): ONE warp with the bilinearly upsampled
//     motion, blend share and confidence. Motion varies continuously from pixel to pixel: no block seams under
//     rotation / zoom. Final fetch Catmull-Rom (gU0.z = 1) or bilinear.
//  2. Motion boundary: each texel's hypothesis is warped (bilinear fetches) and the colours are blended with weights
//     b_k * evidence_k, evidence_k = exp(-0.5 (e_k / 0.08)^2) for hypotheses visible in both frames (e_k = per-pixel
//     colour mismatch of the two warped samples after the local mean-brightness difference), a fixed 0.6 for one-sided
//     hypotheses (unverifiable). The edge lands where the pixel evidence puts it and stays soft where the evidence is
//     ambiguous, instead of flipping per pixel.
//  3. Fallback where confidence is low: plain blend (gP0.w = 0) or the nearer frame (gP0.w = 1);
//     output = lerp(fallback, warped, saturate((conf - 0.1) / 0.5)). Confidence is interpolated, so the transition
//     is smooth (no per-pixel switching between warp and blend).
//  4. Unchanged pixel with an unverifiable moving warp: keep the unchanged colour (see 4b below).
//  5. HUD: lerp(output, lerp(I0(y), I1(y), t), hud(y)).
//  6. Scene cut (GPU stats buffer): the nearer real frame, never a blend.
// Guide (SV_Target1, 0..1): where the generated content is uncertain - motion boundaries, disagreement of the two
// warped samples, partial fallback - for moving, non-HUD pixels only. Zero for static content, the HUD, scene cuts and
// debug views, so real detail and static text are never filtered.
//
//   gSrcSize frame size   gFieldSize selection grid size   gAuxSize mean-luma level size
//   gP0 = (t, cell px, 1 / cell px, fallback mode)
//   gU0 = (debug view, -, cubic warp, -)   gU1.x = hud enable
#include "framegen/fg_common.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PsOut
{
    float4 color : SV_Target0;
    float guide : SV_Target1;
};

Texture2D<float4> gPrev : register(t0);
Texture2D<float4> gCurr : register(t1);
Texture2D<float4> gSel : register(t2);
Texture2D<float2> gSelAux : register(t3);
Texture2D<float> gHud : register(t4);
Buffer<uint> gStats : register(t5);
Texture2D<float> gMeanPrev : register(t6);
Texture2D<float> gMeanCurr : register(t7);
SamplerState gLinear : register(s0);

float3 Hsv2Rgb(float h, float s, float v)
{
    const float3 k = saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return v * lerp(float3(1, 1, 1), k, s);
}

// Catmull-Rom (cubic convolution, a = -0.5) reconstruction at frame position pos, evaluated with 9 bilinear taps:
// per axis the four kernel weights w0..w3 are applied as w0 at texel -1, (w1 + w2) at a fractional position between
// texels 0 and 1 (a bilinear fetch with ratio w2 / (w1 + w2)), and w3 at texel +2. The result is clamped to the
// range of the 2x2 texels around pos so the negative lobes cannot overshoot at hard edges.
float3 SampleCubic(Texture2D<float4> tex, float2 pos, float2 invFrame)
{
    const float2 p1 = floor(pos - 0.5) + 0.5; // centre of texel 0
    const float2 f = pos - p1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 q0 = (p1 - 1.0) * invFrame;
    const float2 q12 = (p1 + w2 / w12) * invFrame;
    const float2 q3 = (p1 + 2.0) * invFrame;
    float3 c = 0;
    c += (tex.SampleLevel(gLinear, float2(q0.x, q0.y), 0).rgb * w0.x + tex.SampleLevel(gLinear, float2(q12.x, q0.y), 0).rgb * w12.x +
          tex.SampleLevel(gLinear, float2(q3.x, q0.y), 0).rgb * w3.x) * w0.y;
    c += (tex.SampleLevel(gLinear, float2(q0.x, q12.y), 0).rgb * w0.x + tex.SampleLevel(gLinear, float2(q12.x, q12.y), 0).rgb * w12.x +
          tex.SampleLevel(gLinear, float2(q3.x, q12.y), 0).rgb * w3.x) * w12.y;
    c += (tex.SampleLevel(gLinear, float2(q0.x, q3.y), 0).rgb * w0.x + tex.SampleLevel(gLinear, float2(q12.x, q3.y), 0).rgb * w12.x +
          tex.SampleLevel(gLinear, float2(q3.x, q3.y), 0).rgb * w3.x) * w3.y;
    const int2 i0 = ClampI(int2(p1), gSrcSize), i1 = ClampI(int2(p1) + 1, gSrcSize);
    const float3 t00 = tex.Load(int3(i0, 0)).rgb, t10 = tex.Load(int3(i1.x, i0.y, 0)).rgb;
    const float3 t01 = tex.Load(int3(i0.x, i1.y, 0)).rgb, t11 = tex.Load(int3(i1, 0)).rgb;
    return clamp(c, min(min(t00, t10), min(t01, t11)), max(max(t00, t10), max(t01, t11)));
}

// Blend share of frame 1 at the pixel, with moving content never read from protected overlay pixels: the share of a
// side whose fetch position lies on the HUD mask is removed (per pixel, since overlay edges are sharp). Returns
// (beta, factor for the confidence: 0 when both fetches are on the overlay).
float2 HudBeta(float beta, float2 x0, float2 x1, float mLen, float2 invFrame)
{
    if (gU1.x == 0) return float2(beta, 1.0);
    const float mv = saturate(mLen - 0.5);
    const float h0 = mv * gHud.SampleLevel(gLinear, x0 * invFrame, 0);
    const float h1 = mv * gHud.SampleLevel(gLinear, x1 * invFrame, 0);
    const float w0 = (1.0 - beta) * (1.0 - h0) + 1e-3;
    const float w1 = beta * (1.0 - h1) + 1e-3;
    return float2(w1 / (w0 + w1), 1.0 - h0 * h1);
}

PsOut main(VsOut i)
{
    PsOut r;
    r.guide = 0.0;
    const float2 y = i.pos.xy;
    const int2 yi = int2(y);
    const float t = gP0.x;
    const float2 frame = float2(gSrcSize);
    const float2 invFrame = 1.0 / frame;
    const float3 a = gPrev.Load(int3(yi, 0)).rgb;
    const float3 b = gCurr.Load(int3(yi, 0)).rgb;
    const uint debugView = gU0.x;

    if (gStats[kStatCut] != 0) {
        r.color = debugView != 0 ? float4(0.6, 0.0, 0.0, 1.0) : float4(t < 0.5 ? a : b, 1.0);
        return r;
    }

    // Selection texel g has its centre at frame position 2 g + 1.
    const float2 gp = (y - 1.0) * 0.5;
    const int2 g0 = int2(floor(gp));
    const float2 fr = gp - float2(g0);
    float4 S[4];
    float2 A[4];
    float wb[4];
    float2 mB = 0.0;
    [unroll] for (int k = 0; k < 4; ++k) {
        const int2 o = int2(k & 1, k >> 1);
        const int2 gi = ClampI(g0 + o, gFieldSize);
        S[k] = gSel.Load(int3(gi, 0));
        A[k] = gSelAux.Load(int3(gi, 0));
        wb[k] = (o.x ? fr.x : 1.0 - fr.x) * (o.y ? fr.y : 1.0 - fr.y);
        mB += wb[k] * S[k].xy;
    }
    float spread = 0.0;
    [unroll] for (int k2 = 0; k2 < 4; ++k2) spread = max(spread, length(S[k2].xy - mB));

    float3 warped;
    float conf, both, agree;
    float2 m;
    float boundary = 0.0;
    [branch] if (spread < 1.0) {
        // 1. smooth region
        m = mB;
        float beta = 0.0;
        conf = 0.0;
        both = 0.0;
        agree = 0.0;
        [unroll] for (int k3 = 0; k3 < 4; ++k3) {
            beta += wb[k3] * S[k3].z;
            conf += wb[k3] * S[k3].w;
            both += wb[k3] * A[k3].x;
            agree += wb[k3] * A[k3].y;
        }
        const float2 x0 = y - t * m;
        const float2 x1 = y + (1.0 - t) * m;
        float3 c0, c1;
        [branch] if (gU0.z != 0) {
            c0 = SampleCubic(gPrev, x0, invFrame);
            c1 = SampleCubic(gCurr, x1, invFrame);
        } else {
            c0 = gPrev.SampleLevel(gLinear, x0 * invFrame, 0).rgb;
            c1 = gCurr.SampleLevel(gLinear, x1 * invFrame, 0).rgb;
        }
        const float2 hb = HudBeta(beta, x0, x1, length(m), invFrame);
        warped = lerp(c0, c1, hb.x);
        conf *= hb.y;
        // Pixel-level disagreement of two visible warped samples (after the local brightness difference): the guide
        // marks it even inside a smooth region (e.g. a thin structure the motion did not capture).
        const float mu = (gMeanPrev.SampleLevel(gLinear, x0 * invFrame, 0) - gMeanCurr.SampleLevel(gLinear, x1 * invFrame, 0)) / 255.0;
        const float e = dot(abs(c0 - c1 - mu), 1.0 / 3.0);
        agree = min(agree, lerp(1.0, exp(-0.5 * (e / 0.25) * (e / 0.25)), both));
    } else {
        // 2. motion boundary: evidence-weighted blend of the four hypotheses
        boundary = saturate((spread - 1.0) / 2.0);
        float3 acc = 0.0;
        float wsum = 0.0, best = -1.0;
        conf = 0.0;
        both = 0.0;
        agree = 0.0;
        m = S[0].xy;
        [unroll] for (int k4 = 0; k4 < 4; ++k4) {
            const float2 mk = S[k4].xy;
            const float2 x0 = y - t * mk;
            const float2 x1 = y + (1.0 - t) * mk;
            const float3 c0 = gPrev.SampleLevel(gLinear, x0 * invFrame, 0).rgb;
            const float3 c1 = gCurr.SampleLevel(gLinear, x1 * invFrame, 0).rgb;
            const float mu = (gMeanPrev.SampleLevel(gLinear, x0 * invFrame, 0) - gMeanCurr.SampleLevel(gLinear, x1 * invFrame, 0)) / 255.0;
            const float e = dot(abs(c0 - c1 - mu), 1.0 / 3.0);
            const float evidence = lerp(0.6, exp(-0.5 * (e / 0.08) * (e / 0.08)), A[k4].x);
            const float w = wb[k4] * (evidence + 0.02);
            const float2 hb = HudBeta(S[k4].z, x0, x1, length(mk), invFrame);
            acc += w * lerp(c0, c1, hb.x);
            conf += w * S[k4].w * hb.y;
            both += w * A[k4].x;
            agree += w * A[k4].y;
            wsum += w;
            if (w > best) {
                best = w;
                m = mk;
            }
        }
        const float inv = 1.0 / max(wsum, 1e-6);
        warped = acc * inv;
        conf *= inv;
        both *= inv;
        agree *= inv;
    }
    const float mLen = length(m);
    // 3. fallback
    const float3 fallback = gP0.w != 0.0 ? (t < 0.5 ? a : b) : lerp(a, b, t);
    const float fb = saturate((conf - 0.1) / 0.5);
    float3 o = lerp(fallback, warped, fb);

    // 4b. Unverifiable warp over an unchanged pixel: if y has the same colour in both frames and the warp for a moving
    // m can only be read from one side (the other is occluded, so c0 / c1 cannot cross-check each other), the
    // unchanged colour is the better-supported hypothesis (typical: flat interior of an overlay that the HUD mask's
    // dilation does not reach, with the scene moving behind it). Content that genuinely crosses y is visible in both
    // frames along its path and is not affected. (Measured 2026-11-16: also preferring "unchanged" where a verifiable
    // warp mismatches cost 0.1-0.4 dB on textured pans; not used.)
    const float3 dab = abs(a - b);
    const float unchanged = 1.0 - smoothstep(2.0 / 255.0, 6.0 / 255.0, max(dab.r, max(dab.g, dab.b)));
    const float keep = unchanged * (1.0 - both) * saturate(mLen - 0.5);
    o = lerp(o, lerp(a, b, t), keep);

    // 5. HUD
    float hud = 0.0;
    if (gU1.x != 0) {
        hud = gHud.Load(int3(yi, 0));
        o = lerp(o, lerp(a, b, t), hud);
    }

    // Anti-aliasing guide.
    const float partial = 4.0 * fb * (1.0 - fb);
    const float uncertain = max(boundary, max(1.0 - agree, partial));
    r.guide = saturate(uncertain * 1.5) * saturate(mLen - 0.5) * (1.0 - hud) * (1.0 - keep);

    if (debugView != 0) {
        r.guide = 0.0;
        if (debugView == 1) {
            const float ang = atan2(m.y, m.x) / 6.2831853 + 0.5;
            r.color = float4(Hsv2Rgb(ang, saturate(mLen / 32.0), 0.25 + 0.75 * saturate(conf)), 1.0);
        } else if (debugView == 2) {
            r.color = float4(conf, conf, conf, 1.0);
        } else {
            const float l = Luma(b) * 0.6;
            r.color = float4(lerp(float3(l, l, l), float3(1.0, 0.0, 1.0), hud), 1.0);
        }
        return r;
    }
    r.color = float4(o, 1.0);
    return r;
}
