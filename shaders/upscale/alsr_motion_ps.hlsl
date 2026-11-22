// Moon ALSR pass 2 (source resolution): per-pixel motion. Owner: upscaling-engineer.
// Drawn over a viewport of exactly the frame size. MRT1 (R8G8) = overlay layer and boundary ring, see Layer().
// Output 0 (RGBA32F) = float4(v, confidence, match error):
// v = where the content of this CURRENT pixel was in the PREVIOUS frame, relative displacement in frame px.
//
// Input: the refined cell field (pass 1; one curr -> prev vector per cellPx x cellPx cell), the blurred luma of the
// current frame (pass 0) and the registration reference of the previous history (alsr_refluma), the estimator's prev -> curr field (slice 0 of
// MotionFieldView::flow) for the visibility test, the HUD mask and the scene-cut flag.
//   1. Smooth candidate: bilinear interpolation of the 2x2 nearest refined cells.
//   2. If the 3x3 nearest refined cells all lie within gP3.z frame px of it, it is used as is (coherent motion:
//      per-pixel selection would only add sub-pixel noise). Otherwise (motion boundary) it, the 3x3 cell vectors and
//      the zero vector are scored by the photometric error of a 5-tap cross on the blurred luma,
//        e = mean |(L_curr(x + d) - m_curr) - (L_prev(x + d + v) - m_prev)| + 0.5 |m_curr - m_prev|
//      plus 0.01 x distance to the cell and 0.03 x (1 - cell confidence); the best wins. (Same principle as the
//      generator's interpolation shader, re-implemented for the curr -> prev direction.) This snaps the cell field to
//      motion boundaries at pixel level instead of smearing it across them (halos).
//   3. Visibility: the content is visible in the previous frame if the prev -> curr field at the landing point points
//      back: vis = exp(-0.5 (|F01(x + v) + v| / tau)^2), tau = gP3.x + gP3.y |v|; landing outside the frame -> 0.
//   4. Confidence = cell confidence x vis x (1 - smoothstep(gP1.z, gP1.w, e)), e = the chosen vector's 5-tap error.
//   4b. |v| < 0.02 frame px -> 0 (exact history copy for static content); unchanged input (5-tap blurred luma equal
//       to the previous frame's within 2e-4) -> 0 (development flag 1024 disables the latter).
//   5. Static HUD (mask, if enabled): v -> 0 and confidence -> 1 by the mask value (zero motion: accumulate).
//   No valid pair / reset / scene cut: v = 0, confidence 0.
#include "upscale/alsr_common.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> gWorkCurr : register(t0);
Texture2D<float4> gWorkPrev : register(t1);
Texture2DArray<float4> gFlow : register(t2);
Texture2D<float> gHud : register(t3);
Buffer<uint> gStats : register(t4);
Texture2D<float4> gCells : register(t5);
Texture2D<float> gRef : register(t6); // registration reference from the previous history (alsr_refluma)

float LPrev(float2 pos)
{
    return (gU1.w & 8u) != 0 ? gWorkPrev.SampleLevel(gLinear, pos * gIn.zw, 0).a : gRef.SampleLevel(gLinear, pos * gIn.zw, 0);
}

int2 CellClamp(int2 c) { return clamp(c, int2(0, 0), int2(gCell.xy) - 1); }

struct PsOut
{
    float4 motion : SV_Target0;
    float2 layer : SV_Target1;
};

// Overlay layer (development flag 2048 disables it): r = 1 on static-overlay pixels (mask >= 0.5), g = 1 on the
// boundary ring = pixels whose 5x5 neighbourhood holds both overlay and non-overlay pixels (anti-aliased / partially
// covered glyph edges, and the background within 2 px of a glyph). Downstream passes keep the two layers apart:
// the ring takes the single-frame reconstruction (no history, no back-projection), so outside the ring every 3x3
// hull, motion tap and residual footprint lies inside one layer.
// r = 0.5 marks a background pixel with an overlay pixel among its 8 neighbours: with anti-aliased input it is
// partially covered by the overlay (its colour mixes both layers), so the background side of LayerCubic does not
// use it. (Consumers test r > 0.75 for "overlay", r < 0.25 for "clean background".)
float2 Layer(int2 px)
{
    if (gU0.y == 0 || (gU1.w & 2048u) != 0) return float2(0, 0);
    // OvlStrength(e) >= 0.5 <=> e >= 128 / 255 (smoothstep is symmetric about its midpoint; e is an R8 level).
    // (Gather variants were measured slower on the RTX 5060 in Stage 3.)
    const int2 inMax = int2(gIn.xy) - 1;
    float anyO = 0, anyB = 0, adj = 0;
    [unroll] for (int k = 0; k < 25; ++k) {
        const int2 o = int2(k % 5 - 2, k / 5 - 2);
        const float h = gHud.Load(int3(clamp(px + o, int2(0, 0), inMax), 0)) >= 127.5 / 255.0 ? 1.0 : 0.0;
        anyO = max(anyO, h);
        anyB = max(anyB, 1.0 - h);
        if (all(abs(o) <= 1)) adj = max(adj, h);
    }
    const bool self = gHud.Load(int3(px, 0)) >= 127.5 / 255.0;
    return float2(self ? 1.0 : 0.5 * adj, anyO * anyB);
}

PsOut Out(float4 m, float2 l)
{
    PsOut o;
    o.motion = m;
    o.layer = l;
    return o;
}

PsOut main(VsOut i)
{
    const int2 px = int2(i.pos.xy);
    if (gU0.x == 0) return Out(float4(0, 0, 0, 1), float2(0, 0));
    if (gU1.y != 0 && gStats[gU1.x] != 0) return Out(float4(0, 0, 0, 1), float2(0, 0)); // scene cut: no history

    const float2 x = i.pos.xy;
    const int2 inMax = int2(gIn.xy) - 1;
    static const float2 kTap[5] = {float2(0, 0), float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1)};
    float yc[5];
    float mc = 0;
    [unroll] for (int k = 0; k < 5; ++k) {
        yc[k] = gWorkCurr.Load(int3(clamp(px + int2(kTap[k]), int2(0, 0), inMax), 0)).a;
        mc += yc[k];
    }
    mc *= 0.2;

    const float2 cellF = x * gCell.w - 0.5;
    const int2 c0 = int2(floor(cellF));
    const float2 cf = cellF - float2(c0);
    const float4 f00 = gCells.Load(int3(CellClamp(c0), 0)), f10 = gCells.Load(int3(CellClamp(c0 + int2(1, 0)), 0));
    const float4 f01 = gCells.Load(int3(CellClamp(c0 + int2(0, 1)), 0)), f11 = gCells.Load(int3(CellClamp(c0 + int2(1, 1)), 0));
    const float4 fBil = lerp(lerp(f00, f10, cf.x), lerp(f01, f11, cf.x), cf.y);

    const int2 base = int2(round(cellF)) - 1;
    float spread = 0;
    [unroll] for (int s = 0; s < 9; ++s)
        spread = max(spread, length(gCells.Load(int3(CellClamp(base + int2(s % 3, s / 3)), 0)).xy - fBil.xy));

    const bool select = spread >= gP3.z || (gU1.w & 4u) != 0; // development flag 4: always select
    float4 best = fBil;
    float bestErr = 0, bestScore = 1e9;
    [loop] for (int n = 0; n < (select ? 11 : 1); ++n) {
        float4 f;
        float dist = 0.0;
        if (n == 0) {
            f = fBil;
        } else if (n < 10) {
            const int2 c = CellClamp(base + int2(uint(n - 1) % 3u, uint(n - 1) / 3u));
            f = gCells.Load(int3(c, 0));
            dist = length(cellF - float2(c));
        } else {
            f = float4(0, 0, 0.5, 0); // zero-motion candidate (static content inside a moving cell)
            dist = 1.0;
        }
        float yp[5];
        float mp = 0;
        [unroll] for (int k2 = 0; k2 < 5; ++k2) {
            yp[k2] = LPrev(x + kTap[k2] + f.xy);
            mp += yp[k2];
        }
        mp *= 0.2;
        float e = 0;
        [unroll] for (int k3 = 0; k3 < 5; ++k3) e += abs(yc[k3] - mc - (yp[k3] - mp));
        e = e * 0.2 + 0.5 * abs(mc - mp);
        const float score = e + 0.01 * dist + 0.03 * (1.0 - f.z);
        if (score < bestScore) {
            best = f;
            bestErr = e;
            bestScore = score;
        }
    }

    const float2 v = best.xy;
    const float2 land = x + v;
    float vis = 0.0;
    if (all(land >= 0.0) && all(land <= gIn.xy)) {
        const int2 lc = CellClamp(int2(floor(land * gCell.w)));
        const float2 back = gFlow.Load(int4(lc, 0, 0)).xy;
        const float tau = gP3.x + gP3.y * length(v);
        const float ev = length(back + v) / tau;
        vis = exp(-0.5 * ev * ev);
    }
    float conf = saturate(best.z) * vis * (1.0 - smoothstep(gP1.z, gP1.w, bestErr));
    // Sub-0.02 px vectors are snapped to zero: a static scene must reproject as an exact copy (no resampling), and
    // estimation noise of that size is below what accumulation can use. (Measured: without the snap a few pixels of a
    // static scene kept changing by up to 3 levels on the Radeon iGPU.)
    float2 vo = length(v) < 0.02 ? float2(0, 0) : v;
    if ((gU1.w & 1024u) == 0) {
        // Unchanged input: where the 5-tap blurred luma equals the previous FRAME's (not the history reference's)
        // within 2e-4 (0.05 of an 8-bit level), nothing moved; v = 0 exactly. Needed because at non-integer scales the
        // registration reference (resampled history) differs slightly from the frame it came from and Lucas-Kanade
        // turns that into a few hundredths of a pixel of false motion on static content (tbench static 1.5x).
        float e0 = 0;
        [unroll] for (int k4 = 0; k4 < 5; ++k4)
            e0 += abs(yc[k4] - gWorkPrev.Load(int3(clamp(px + int2(kTap[k4]), int2(0, 0), inMax), 0)).a);
        if (e0 * 0.2 < 2e-4) vo = float2(0, 0);
    }
    if (gU0.y != 0) {
        const float h = OvlStrength(gHud.Load(int3(px, 0)));
        vo *= 1.0 - h;
        conf = lerp(conf, 1.0, h);
    }
    return Out(float4(vo, conf, bestErr), Layer(px));
}
