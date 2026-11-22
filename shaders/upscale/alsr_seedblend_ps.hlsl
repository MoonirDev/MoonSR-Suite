// Moon ALSR pass 3b (output resolution): single-frame reconstruction where the history is not fully trusted.
// Owner: upscaling-engineer. Drawn right after alsr_reproject into its MRT0 (Hw) with ADDITIVE blending on RGB
// (alpha not written): Hw.rgb += (1 - trust) x base, so that Hw = lerp(base, clipped history, trust) (alsr_reproject
// wrote trust x clipped history). Pixels with trust >= 1 never run this shader: alsr_reproject writes depth 1 where
// trust < 1 and 0 elsewhere, and this pass is drawn at z = 0 with an early depth test LESS - with tracked history that
// is most of the frame, which is the point of this pass (Stage 3 cost cut, see alsr_reproject_ps.hlsl). The trust test
// below is kept as a safety net.
//   base = the seed (MoonSR Spatial + learned filters, alsr_seed.hlsli), working space;
//          on the overlay boundary ring (alsr_motion Layer(), any of the 2x2 source pixels around x) the two layers
//          reconstructed separately (LayerCubic) and composited with the bilinear overlay coverage at x (development
//          flag 4096: the seed; 8192: the layer of the nearest source pixel only).
// t0 = trust (alsr_reproject MRT1), t1 = working copy, t2 = layer, t3 = frame (gamma), t4 = filter table.
// Development flag 131072 (cost measurement only): no seed at all (discard everything).
#include "upscale/alsr_common.hlsli"
#define ALSR_SEED_SRC_REG t3
#define ALSR_SEED_TABLE_REG t4
#include "upscale/alsr_seed.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float> gTrust : register(t0);
Texture2D<float4> gWork : register(t1);   // current frame, working space (source res)
Texture2D<float2> gLayer : register(t2);  // overlay layer / boundary ring (alsr_motion MRT1, source res)

// Layer-separated single-frame reconstruction for the overlay boundary ring: Catmull-Rom over the 4x4 source pixels
// around x, with the weights of pixels of the other layer set to zero and the rest renormalised (normalised
// convolution; Knutsson & Westin 1993). A glyph's colour does not bleed into the background next to it and vice versa,
// which no single-frame filter that ignores the layers can avoid. The output pixel's layer is that of its nearest
// source pixel. Falls back to `fallback` when too little weight remains. Result clamped to the used pixels' range.
float3 LayerCubic(float2 x, bool overlay, float3 fallback)
{
    const float2 p1 = floor(x - 0.5) + 0.5;
    const float2 f = x - p1;
    float wx[4], wy[4];
    [unroll] for (int k = 0; k < 4; ++k) {
        const float2 d = abs(float2(k - 1, k - 1) - f);
        const float2 w = d < 1.0 ? (1.5 * d - 2.5) * d * d + 1.0 : ((-0.5 * d + 2.5) * d - 4.0) * d + 2.0;
        wx[k] = w.x;
        wy[k] = w.y;
    }
    const int2 b = int2(p1) - 1;
    const int2 hiI = int2(gIn.xy) - 1;
    float3 col[16];
    float lxs[16];
    float3 mO = 0, mB = 0;
    float nO = 0, nB = 0;
    [unroll] for (int t = 0; t < 16; ++t) {
        const int2 s = clamp(b + int2(t % 4, t / 4), int2(0, 0), hiI);
        col[t] = gWork.Load(int3(s, 0)).rgb;
        lxs[t] = gLayer.Load(int3(s, 0)).x;
        if (lxs[t] > 0.75) { mO += col[t]; nO += 1.0; }
        else if (lxs[t] < 0.25) { mB += col[t]; nB += 1.0; }
    }
    mO /= max(nO, 1.0);
    mB /= max(nB, 1.0);
    const float3 dOB = mB - mO;
    const float dd = dot(dOB, dOB);
    float3 acc = 0, lo = 1e9, hi = -1e9;
    float wsum = 0, wpos = 0;
    [unroll] for (int j = 0; j < 4; ++j) {
        [unroll] for (int i2 = 0; i2 < 4; ++i2) {
            const int t2 = j * 4 + i2;
            const float lx = lxs[t2];
            const float3 c = col[t2];
            // Overlay side: overlay pixels. Background side: clean background, plus the background pixels next to an
            // overlay weighted by how clearly they are background: anti-aliased input mixes them with the overlay.
            // Their background fraction is estimated by projecting the colour onto the segment between the window's
            // mean overlay and mean clean-background colours (alpha estimation as in colour-line matting): hard-edged
            // input gives ~1 (kept), a half-covered pixel ~0.5 (dropped). Development flags: 32768 keeps them always,
            // 65536 drops them always.
            float q = 0.0;
            if (overlay) {
                q = lx > 0.75 ? 1.0 : 0.0;
            } else if (lx < 0.25) {
                q = 1.0;
            } else if (lx < 0.75) {
                if ((gU1.w & 32768u) != 0 || nB == 0.0 || nO == 0.0 || dd < 1e-4) q = 1.0;
                else q = smoothstep(0.55, 0.85, dot(c - mO, dOB) / dd);
                if ((gU1.w & 65536u) != 0) q = 0.0;
            }
            if (q > 0.0) {
                const float w = wx[i2] * wy[j] * q;
                acc += w * c;
                wsum += w;
                wpos += max(w, 0.0);
                lo = min(lo, c);
                hi = max(hi, c);
            }
        }
    }
    if (wsum < 0.35 || wpos < 0.35) return fallback;
    return clamp(acc / wsum, lo, hi);
}

[earlydepthstencil]
float4 main(VsOut i) : SV_Target
{
    const int2 p = int2(i.pos.xy);
    const float trust = gTrust.Load(int3(p, 0));
    if (trust >= 1.0 || (gU1.w & 131072u) != 0) discard;
    float alpha;
    const float3 spatial = ToWork(saturate(AlsrSeed(uint2(p), alpha)));
    float3 base = spatial;
    if (gU0.x != 0 && (gU1.w & 4096u) == 0) {
        const float2 x = i.pos.xy * gScale.zw;
        const float2 xb = x - 0.5;
        const int2 b0 = int2(floor(xb));
        const float2 bf = xb - float2(b0);
        const int2 hiI = int2(gIn.xy) - 1;
        const float2 l00 = gLayer.Load(int3(clamp(b0, int2(0, 0), hiI), 0)), l10 = gLayer.Load(int3(clamp(b0 + int2(1, 0), int2(0, 0), hiI), 0));
        const float2 l01 = gLayer.Load(int3(clamp(b0 + int2(0, 1), int2(0, 0), hiI), 0)), l11 = gLayer.Load(int3(clamp(b0 + int2(1, 1), int2(0, 0), hiI), 0));
        if (max(max(l00.y, l10.y), max(l01.y, l11.y)) > 0.5) {
            const float a = lerp(lerp(step(0.75, l00.x), step(0.75, l10.x), bf.x), lerp(step(0.75, l01.x), step(0.75, l11.x), bf.x), bf.y);
            if ((gU1.w & 8192u) != 0) {
                const int2 xi = clamp(int2(x), int2(0, 0), hiI);
                base = LayerCubic(x, gLayer.Load(int3(xi, 0)).x > 0.75, spatial);
            } else {
                const float3 co = a > 0.0 ? LayerCubic(x, true, spatial) : 0;
                const float3 cb = a < 1.0 ? LayerCubic(x, false, spatial) : 0;
                base = lerp(cb, co, a);
            }
        }
    }
    return float4(base * (1.0 - trust), 0.0);
}
