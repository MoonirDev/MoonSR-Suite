// Moon ALSR pass 3 (output resolution): reproject, rectify and gate the history. Owner: upscaling-engineer.
// MRT: SV_Target0 (RGBA16F) = Hw: rgb = predicted colour for this frame (working space), a = accumulated count after
//      rejection. SV_Target1 (R8) = history trust 0..1 (debug view and the update pass).
//
// Per output pixel p (centre y = p + 0.5, source position x = y * source/output):
//   1. Motion: the per-pixel vectors of pass 1 at x - bilinear among the 2x2 nearest source pixels when they agree
//      within gP3.w source px, else the nearest one - scaled to output px. Previous output position q = y + v * out/src.
//   2. History colour: Catmull-Rom (9 bilinear taps) of the previous history at q, clamped to its 2x2 neighbours
//      (resampling is repeated every frame, so overshoot would accumulate). Count: bilinear history alpha at q.
//   3. Rectification: the 3x3 source neighbourhood of x in YCoCg gives mean m and the min / max hull; the history is
//      clipped along the ray towards m onto the hull expanded by gP4.w x its range (ray-box clip, not a per-channel
//      clamp, so hue is preserved), the box shrunk towards m in fast motion (see below).
//   4. Trust: smoothstep(confLo, confHi, confidence) x [q inside the output] x [history valid, no scene cut]
//      x (1 - max(0, hud(landing) - hud(x))) (no overlay colour into revealed background).
//   4b. Overlay boundary ring (alsr_motion Layer()): trust 0 (the base is chosen by alsr_seedblend).
//   5. Hw = trust x clipped history (premultiplied); count = history count x trust. The next pass, alsr_seedblend, adds
//      (1 - trust) x the single-frame reconstruction with additive blending, only where trust < 1, so that
//      Hw = lerp(single-frame reconstruction, clipped history, trust) as before Stage 3. (Stage 3 cost cut: the
//      single-frame reconstruction was two full output-resolution passes every frame, 0.42 ms of 1.86 ms on the RTX
//      5060 at 720p -> 1080p; fused into this shader it cost 0.19 ms through register pressure even where unused.)
#include "upscale/alsr_common.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PsOut
{
    float4 hw : SV_Target0;
    float trust : SV_Target1;
#ifdef ALSR_ROBUST_HISTORY
    // Lifetime, lock shading reference, current fine luma, previous fine luma.
    float4 state : SV_Target2;
#endif
    float depth : SV_Depth; // 1 where trust < 1 (alsr_seedblend runs there: early depth test LESS at z = 0), else 0
};

Texture2D<float4> gHist : register(t0);   // previous history (output res)
Texture2D<float4> gMotion : register(t1); // pass 1 (source res)
Texture2D<float4> gWork : register(t2);   // current frame, working space (source res)
#ifdef ALSR_ROBUST_HISTORY
Texture2D<float4> gHistoryState : register(t3);
#endif
#ifdef ALSR_STATIC_FOCUS
Texture2D<float4> gPreviousWork : register(t3);
Texture2D<float4> gSampling : register(t7);
#endif
Buffer<uint> gStats : register(t4);
Texture2D<float> gHud : register(t5);     // static-overlay mask of this pair (source res), may be unbound
Texture2D<float2> gLayer : register(t6);  // overlay layer / boundary ring (alsr_motion MRT1, source res)

// Clip c along the ray from m (inside the box) onto the axis-aligned box [bl, bh] (YCoCg). Returns c if inside.
float3 ClipToBox(float3 c, float3 m, float3 bl, float3 bh)
{
    const float3 d = c - m;
    const float3 lim = d > 0.0 ? bh - m : m - bl;
    const float3 r = abs(d) / max(lim, 1e-5);
    const float t = max(r.x, max(r.y, r.z));
    return t > 1.0 ? m + d / t : c;
}

PsOut EmptyHistory()
{
    PsOut o;
    o.hw = float4(0.0, 0.0, 0.0, 0.0);
    o.trust = 0.0;
#ifdef ALSR_ROBUST_HISTORY
    o.state = 0.0;
#endif
    o.depth = 1.0;
    return o;
}

float HistoryTrust(float4 mv, float2 x, int2 xi, int2 b0, bool inside)
{
    float trust = smoothstep(gP1.x, gP1.y, mv.z);
    if (!inside) trust = 0.0;
    // Overlay boundary ring (any of the 2x2 source pixels around x): the single-frame reconstruction, no history.
    // Mixing the two layers there (history of one, hull / motion of the other) is what drew dark outlines around
    // static HUD glyphs while the scene moved behind them.
    // (Gathers at the texel corner b0 + 1 return the 2x2 texels b0 .. b0 + 1, clamped to the edge like the Loads.)
    const float4 ringG = gLayer.GatherGreen(gPoint, float2(b0 + 1) * gIn.zw);
    const float ring = max(max(ringG.x, ringG.y), max(ringG.z, ringG.w));
    if (ring > 0.5) trust = 0.0;
    // Background revealed next to a static overlay: its history position lies on the overlay, whose colour is also
    // inside the local hull and would survive the clip as a ghost. History that lands on an overlay pixel is only
    // valid for overlay pixels.
    if (gU0.y != 0) {
        const float hHere = OvlStrength(gHud.Load(int3(xi, 0)));
        const float hThere = OvlStrength(gHud.SampleLevel(gLinear, (x + mv.xy) * gIn.zw, 0));
        trust *= 1.0 - saturate(hThere - hHere);
        // The history fetch (Catmull-Rom, +-2 output px around q) must not reach into the boundary ring or the other
        // layer at the previous position: there the history holds the other layer's colour (development flag 16384
        // disables this test).
        if ((gU1.w & 16384u) == 0 && any(mv.xy != 0.0)) {
            const float2 lc = (floor(x + mv.xy - 0.5) + 1.0) * gIn.zw;
            const float4 t = max(gLayer.GatherRed(gPoint, lc), gLayer.GatherGreen(gPoint, lc));
            if (max(max(t.x, t.y), max(t.z, t.w)) > 0.5) trust = 0.0;
        }
    }
    return trust;
}

PsOut main(VsOut i)
{
    PsOut o;
    const int2 p = int2(i.pos.xy);
    const float2 y = i.pos.xy;
    const float2 x = y * gScale.zw;
    const int2 xi = clamp(int2(x), int2(0, 0), int2(gIn.xy) - 1);

    bool valid = gU0.x != 0;
    if (valid && gU1.y != 0 && gStats[gU1.x] != 0) valid = false;
#ifdef ALSR_STATIC_FOCUS
    const float4 stationary = gSampling.Load(int3(0, 0, 0));
    float still = stationary.z;
    [branch] if (valid && still > 0.0) {
        // Small animations can fall between the global detector's sample positions.
        // Keep the local pixel on the moving path when its actual source sample changed.
        const float3 difference = abs(gWork.Load(int3(xi, 0)).rgb - gPreviousWork.Load(int3(xi, 0)).rgb);
        if (any(difference > 0.002)) still = 0.0;
    }
    [branch] if (valid && still == 1.0 && stationary.w >= 12.0) {
        // The previous output already used the full stationary filter. Reuse the history
        // at its exact texel centres while new samples continue to match.
        o.hw = gHist.Load(int3(p, 0));
        o.trust = 1.0;
        o.depth = 0.0;
        return o;
    }
    valid = valid && still < 1.0;
#endif
    if (!valid) return EmptyHistory();

    // Per-pixel motion at x: bilinear among the 2x2 nearest source pixels where their vectors agree within gP3.w
    // source px (smooth sub-pixel motion), otherwise the nearest one (keeps motion boundaries sharp).
    const float2 xb = x - 0.5;
    const int2 b0 = int2(floor(xb));
    const float2 bf = xb - float2(b0);
    const int2 hiI = int2(gIn.xy) - 1;
    const float4 m00 = gMotion.Load(int3(clamp(b0, int2(0, 0), hiI), 0));
    const float4 m10 = gMotion.Load(int3(clamp(b0 + int2(1, 0), int2(0, 0), hiI), 0));
    const float4 m01 = gMotion.Load(int3(clamp(b0 + int2(0, 1), int2(0, 0), hiI), 0));
    const float4 m11 = gMotion.Load(int3(clamp(b0 + int2(1, 1), int2(0, 0), hiI), 0));
    const float4 mb = lerp(lerp(m00, m10, bf.x), lerp(m01, m11, bf.x), bf.y);
#ifdef ALSR_FAST_REPROJECT
    const float2 d00 = m00.xy - mb.xy, d10 = m10.xy - mb.xy;
    const float2 d01 = m01.xy - mb.xy, d11 = m11.xy - mb.xy;
    const float dev = sqrt(max(max(dot(d00, d00), dot(d10, d10)), max(dot(d01, d01), dot(d11, d11))));
#else
    const float dev = max(max(length(m00.xy - mb.xy), length(m10.xy - mb.xy)), max(length(m01.xy - mb.xy), length(m11.xy - mb.xy)));
#endif
    const float4 mv = dev < gP3.w ? mb : gMotion.Load(int3(xi, 0));
    const float2 q = y + mv.xy * gScale.xy;
    const bool inside = all(q >= 0.0) && all(q <= gOut.xy);
    // Development flags: 64 = no 2x2 clamp on the history fetch, 128 = sharper cubic (a = -0.75).
    float4 h;
    float count;
#ifdef ALSR_FAST_REPROJECT
    const float trust = HistoryTrust(mv, x, xi, b0, inside);
    [branch] if (trust <= 0.0) return EmptyHistory();
    // With zero motion, Catmull-Rom and bilinear alpha both reduce to the current history texel.
    [branch] if (all(mv.xy == 0.0)) {
        h = gHist.Load(int3(p, 0));
        count = h.a;
    } else
#endif
    {
        h = SampleCubicA(gHist, q, gOut.zw, (gU1.w & 64u) == 0, (gU1.w & 128u) != 0 ? -0.75 : -0.5);
        count = gHist.SampleLevel(gLinear, q * gOut.zw, 0).a;
    }

    // Neighbourhood statistics of the current frame (3x3 source pixels).
    float3 m1 = 0, lo = 1e9, hi = -1e9;
#ifdef ALSR_ROBUST_HISTORY
    float luma[9];
#endif
    [unroll] for (int j = 0; j < 9; ++j) {
        const int2 s = clamp(xi + int2(j % 3 - 1, j / 3 - 1), int2(0, 0), int2(gIn.xy) - 1);
        const float3 c = RgbToYCoCg(gWork.Load(int3(s, 0)).rgb);
        m1 += c;
#ifdef ALSR_ROBUST_HISTORY
        luma[j] = c.x;
#endif
        lo = min(lo, c);
        hi = max(hi, c);
    }
    m1 /= 9.0;
    // Box: the 3x3 min / max hull expanded by gP4.w x its range, shrunk towards the mean as the motion grows (factor
    // gP2.z / gP0.z at gP2.w source px). The variance box (mean +- k sigma) was measured first: at high-contrast edges
    // it is wider than the hull and let overshoot through (dark fringes on the HUD glyph edges of tbench hud_pan).
    const float motionSrc = length(mv.xy);
    const float shrink = lerp(1.0, gP2.z / max(gP0.z, 1e-3), saturate(motionSrc / max(gP2.w, 1e-3)));
    const float3 mr = gP4.w * (hi - lo) + 1e-4;
    const float3 bl = m1 - shrink * (m1 - lo + mr), bh = m1 + shrink * (hi - m1 + mr);
    float3 hc = YCoCgToRgb(ClipToBox(RgbToYCoCg(h.rgb), m1, bl, bh));

#ifndef ALSR_FAST_REPROJECT
    float trust = HistoryTrust(mv, x, xi, b0, inside);
#endif
#ifdef ALSR_ROBUST_HISTORY
    // A sign-consistent extremum seen across opposite neighbours is a thin-line candidate.
    // Track it briefly through missed sample phases; a changed low-pass shade or visibility kills the lock.
    const float4 prev = gHistoryState.SampleLevel(gPoint, q * gOut.zw, 0);
    const float4 side = luma[4] - float4(luma[3], luma[5], luma[1], luma[7]);
    const float4 signedSide = side * (luma[4] >= m1.x ? 1.0 : -1.0);
    const float thin = max(min(signedSide.x, signedSide.y), min(signedSide.z, signedSide.w));
    const float contrast = hi.x - lo.x;
    const float visibility = smoothstep(0.65, 0.95, trust) *
                             (1.0 - smoothstep(0.2, 0.7, dev));
    const float shadeMatch = 1.0 - smoothstep(0.035, 0.12, abs(m1.x - prev.y));
    const float newborn = smoothstep(0.045, 0.15, thin) * smoothstep(0.06, 0.20, contrast);
    const float carry = max(prev.x - 0.25, 0.0) * shadeMatch;
    const float lock = max(carry, newborn) * visibility;
    const float lockShade = carry > newborn && lock > 0.0 ? prev.y : m1.x;

    // Alternate-sign luma differences that return close to the sample two frames ago indicate
    // oscillation. Relax rectification only on coherent motion and when the shading still agrees.
    const float delta = luma[4] - prev.z;
    const float oldDelta = prev.z - prev.w;
    const float oscillation = (delta * oldDelta < 0 ? 1.0 : 0.0) * smoothstep(0.02, 0.10, abs(delta))
        * (1.0 - smoothstep(0.015, 0.065, abs(luma[4] - prev.w))) * visibility * shadeMatch;

    // Use distance outside the supported colour range rather than penalising all high-contrast
    // history. Normalize by local spread so a small excursion in a flat region is less trusted.
    const float3 history = RgbToYCoCg(h.rgb);
    const float3 range = hi - lo;
    const float3 spread = 0.16 * range * range;
    const float3 excursion = max(max(lo - history, history - hi), 0.0);
    const float3 distance = excursion * rsqrt(spread + float3(0.0025, 0.0016, 0.0016));
    const float credibility = exp2(-dot(distance, distance) * 0.75);
    // A repeating signal alone should not override a large mismatch to the current
    // neighborhood. A validated line lock can still cover missing sample phases.
    const float protection = max(lock, 0.55 * oscillation * smoothstep(0.20, 0.80, credibility));
    const float retain = (0.78 + 0.22 * credibility) * protection;
    hc = lerp(hc, h.rgb, retain);
    // Leave full coverage exactly at one unless the excursion is large enough to reject history.
    // Small excursions must not turn the depth-masked seed blend into a full-screen pass.
    const float rejection = 0.45 * (1.0 - smoothstep(0.10, 0.55, credibility));
    trust *= 1.0 - rejection * (1.0 - protection);
    o.state = float4(lock, lockShade, luma[4], trust > 0.0 ? prev.z : luma[4]);
#endif
#ifdef ALSR_STATIC_FOCUS
    trust *= 1.0 - still;
#endif
    o.hw = float4(hc * trust, count * trust);
    o.trust = trust;
    o.depth = trust < 1.0 ? 1.0 : 0.0;
    return o;
}
