// Smart parallax, Apply (once per displayed frame, output resolution): relief shading T1. Owner: upscaling-engineer.
// Research note 2.1 and 2.5. Reads the game-resolution height H and the half-size protection gates (bilinear, so they are upsampled
// smoothly; both are already edge-aware: H comes from the guided-filter detail layer and P is zero across geometry
// edges, HUD and flat regions) and modulates luminance only, in linear light:
//   N      = normalize(-bump * dH/dx, -bump * dH/dy, 1)        (dH from 4 diagonal bilinear taps, game px)
//   shade  = tanh(-bump * grad(H) . L.xy)                       (0 on flat areas; slopes facing L brighten, others darken)
//   cavity = max(0, mean4(H) - H)                              (pixels lower than their neighbourhood darken)
//   delta  = maxDelta * tanh(strength * (shading*Ks*shade - cavity*Kc*cavity) / maxDelta) * P * fade
//   out    = srgb(linear(src) * (1 + delta))                   (all three channels scaled: chroma kept)
// maxDelta is 15 % at any strength (soft clip), so the change never exceeds about +-15 % of the pixel's
// linear luminance. Where delta is 0 (protected, letterbox, Split left half) the source pixel is returned unchanged,
// bit for bit. For generated frames (phase < 1) H and P are fetched at the position the content has in the newest
// real frame, using the prev -> curr cell motion scaled by (1 - phase), when a motion field was supplied.
#define RELIEF_NO_ANALYSE_CB
#include "relief/relief_common.hlsli"

cbuffer ReliefApplyCB : register(b0)
{
    float4 aContent;    // content rect in dst px: x, y, width, height
    float2 aGameSize;   // analysed size (game px)
    float2 aGameTexel;  // 1 / aGameSize
    float4 aLight;      // xyz: unit light vector in screen space (x right, y DOWN, z towards the viewer)
    float  aShadeGain;  // strength * shading * Ks
    float  aCavityGain; // strength * cavity * Kc
    float  aMaxDelta;
    float  aBump;
    uint   aDebug;      // ReliefDebugView value
    float  aSplitX;     // dst x of the Split divider (Split view only)
    float  aWarp;       // (1 - phase) when motion is available, else 0
    uint   aSrcLinear;  // 1: the SRV returns linear values (sRGB view or float format): no conversion
    float2 aCellInv;    // 1 / cell size (game px)
    uint2  aCells;
    float  aTapPx;      // height-tap distance in game px (scales with the detail size: borders bevel, grain does not)
    float3 aPad;
};

Texture2D<float4>      srcTex   : register(t0);
Texture2D<float>       hTex     : register(t1);
Texture2D<float4>      pTex     : register(t2); // half-size gates (hud, edge * tex, changed, SigmaEnc(sigma))
ByteAddressBuffer      state    : register(t3);
Texture2DArray<float4> flowTex  : register(t4);
SamplerState           linClamp : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

#define DBG_NONE 0
#define DBG_HEIGHT 1
#define DBG_NORMALS 2
#define DBG_PROTECTION 3
#define DBG_SPLIT 6

float4 main(VsOut i) : SV_Target
{
    const int2 px = int2(i.pos.xy);
    const float4 src = srcTex.Load(int3(px, 0));
    const float2 local = i.pos.xy - aContent.xy;
    if (any(local < 0.0) || any(local >= aContent.zw)) return src; // letterbox
    if (aDebug == DBG_SPLIT && abs(i.pos.x - aSplitX) < 1.0) return float4(0.5, 0.5, 0.5, src.a); // divider
    if (aDebug == DBG_SPLIT && i.pos.x < aSplitX) return src;

    float2 g = local / aContent.zw * aGameSize; // game px
    if (aWarp > 0.0) {
        const int2 cell = min(int2(g * aCellInv), int2(aCells) - 1);
        g += aWarp * flowTex.Load(int4(cell, 0, 0)).xy; // slice 0: prev -> curr
    }
    const float2 uv = g * aGameTexel;
    const float2 t = aGameTexel * aTapPx;
    float4 prot = pTex.SampleLevel(linClamp, uv, 0);
    const float4 hud4 = pTex.GatherRed(linClamp, uv); // HUD gate: the minimum of the 4 texels, so an overlay border
    prot.x = min(min(hud4.x, hud4.y), min(hud4.z, hud4.w)); // never receives a bilinear share of unprotected neighbours
    prot.w = prot.x * prot.y; // P = HUD gate x (edge gate x texture gate)
    const float fade = asfloat(state.Load(RST_FADE));
    // Protected, faded-out or unprocessed pixels skip the height taps (normal output only; debug views need them).
    if ((aDebug == DBG_NONE || aDebug == DBG_SPLIT) && prot.w * fade <= 0.0) return src;
    const float h0 = hTex.SampleLevel(linClamp, uv, 0);
    const float hA = hTex.SampleLevel(linClamp, uv + float2(t.x, t.y), 0);
    const float hB = hTex.SampleLevel(linClamp, uv + float2(-t.x, t.y), 0);
    const float hC = hTex.SampleLevel(linClamp, uv + float2(t.x, -t.y), 0);
    const float hD = hTex.SampleLevel(linClamp, uv + float2(-t.x, -t.y), 0);

    const float dhx = 0.25 * ((hA + hC) - (hB + hD));
    const float dhy = 0.25 * ((hA + hB) - (hC + hD));
    const float3 n = normalize(float3(-aBump * dhx, -aBump * dhy, 1.0));
    // Directional term, symmetric for slopes facing towards and away from the light (N.L - L.z is not: a slope can
    // darken by up to L.z but brighten only by 1 - L.z), so light-facing bevel edges get a highlight as strong as the
    // shadow on the opposite edge.
    const float shade = tanh(-aBump * dot(float2(dhx, dhy), aLight.xy));
    const float cavity = max(0.0, 0.25 * (hA + hB + hC + hD) - h0);

    if (aDebug == DBG_HEIGHT) return float4((0.5 + 0.5 * h0).xxx, src.a);
    if (aDebug == DBG_NORMALS) return float4(0.5 + 0.5 * n, src.a);
    if (aDebug == DBG_PROTECTION) // red = HUD-protected, blue = edge-protected or no texture, green = receives relief
        return float4(0.25 * src.rgb + 0.75 * float3(1.0 - prot.x, prot.w, 1.0 - prot.y), src.a);

    const float d = aShadeGain * shade - aCavityGain * cavity;
    const float delta = aMaxDelta * tanh(d / aMaxDelta) * prot.w * fade;
    if (abs(delta) < (1.0 / 2048.0)) return src; // below 8-bit visibility: keep the pixel exact
    if (aSrcLinear) return float4(src.rgb * (1.0 + delta), src.a);
    const float3 lin = SrgbToLinear3(saturate(src.rgb)) * (1.0 + delta);
    return float4(LinearToSrgb3(saturate(lin)), src.a);
}
