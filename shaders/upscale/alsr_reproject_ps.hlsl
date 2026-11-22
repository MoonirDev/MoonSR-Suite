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
    float depth : SV_Depth; // 1 where trust < 1 (alsr_seedblend runs there: early depth test LESS at z = 0), else 0
};

Texture2D<float4> gHist : register(t0);   // previous history (output res)
Texture2D<float4> gMotion : register(t1); // pass 1 (source res)
Texture2D<float4> gWork : register(t2);   // current frame, working space (source res)
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

PsOut main(VsOut i)
{
    PsOut o;
    const int2 p = int2(i.pos.xy);
    const float2 y = i.pos.xy;
    const float2 x = y * gScale.zw;
    const int2 xi = clamp(int2(x), int2(0, 0), int2(gIn.xy) - 1);

    bool valid = gU0.x != 0;
    if (valid && gU1.y != 0 && gStats[gU1.x] != 0) valid = false;
    if (!valid) {
        o.hw = float4(0.0, 0.0, 0.0, 0.0);
        o.trust = 0.0;
        o.depth = 1.0;
        return o;
    }

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
    const float dev = max(max(length(m00.xy - mb.xy), length(m10.xy - mb.xy)), max(length(m01.xy - mb.xy), length(m11.xy - mb.xy)));
    const float4 mv = dev < gP3.w ? mb : gMotion.Load(int3(xi, 0));
    const float2 q = y + mv.xy * gScale.xy;
    const bool inside = all(q >= 0.0) && all(q <= gOut.xy);
    // Development flags: 64 = no 2x2 clamp on the history fetch, 128 = sharper cubic (a = -0.75).
    float4 h = SampleCubicA(gHist, q, gOut.zw, (gU1.w & 64u) == 0, (gU1.w & 128u) != 0 ? -0.75 : -0.5);
    const float count = gHist.SampleLevel(gLinear, q * gOut.zw, 0).a;

    // Neighbourhood statistics of the current frame (3x3 source pixels).
    float3 m1 = 0, lo = 1e9, hi = -1e9;
    [unroll] for (int j = 0; j < 9; ++j) {
        const int2 s = clamp(xi + int2(j % 3 - 1, j / 3 - 1), int2(0, 0), int2(gIn.xy) - 1);
        const float3 c = RgbToYCoCg(gWork.Load(int3(s, 0)).rgb);
        m1 += c;
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
    const float3 hc = YCoCgToRgb(ClipToBox(RgbToYCoCg(h.rgb), m1, bl, bh));

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
    o.hw = float4(hc * trust, count * trust);
    o.trust = trust;
    o.depth = trust < 1.0 ? 1.0 : 0.0;
    return o;
}
