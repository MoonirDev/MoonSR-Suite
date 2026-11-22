// Moon ALSR pass 0 (source resolution): working-space copy of the current frame + blurred luma.
// Owner: upscaling-engineer. Output (RGBA16F): rgb = current colour in the working space (gamma or linear light,
// gU0.z), a = luma (BT.709 weights on the gamma-encoded values) filtered with a 5x5 binomial kernel (sigma 1 px).
// The blurred luma is what motion refinement matches on (it damps sampling noise and the highest aliased
// frequencies; it cannot remove aliasing that has already folded into low frequencies). The upscaler keeps two of
// these textures, so the previous frame's blurred luma is available without recomputation.
#include "upscale/alsr_common.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> gCurr : register(t0); // BGRA8 frame
Texture2D<float4> gWorkPrev : register(t1); // previous frame's output of this pass (valid when gU2.y != 0)
Texture2D<float> gHud : register(t2);       // generator's static-overlay mask of this pair (null = HUD protection off)
Texture2D<float> gOvlPrev : register(t3);   // previous persistent overlay mask (MRT1 of this pass)

struct PsOut
{
    float4 work : SV_Target0;
    float ovl : SV_Target1;
};

// Persistent overlay evidence (MRT1, R8; see OvlStrength in alsr_common.hlsli): the generator decides per pair
// whether a pixel is a static overlay. That test misses some glyph strokes in some frames (e.g. a stroke lying on a
// background line moving along it) and fires on isolated pixels of slowly moving aliased texture. An overlay is
// persistent, so evidence accumulates over frames in which the pixel is unchanged in place (max channel difference
// to the previous frame below 1.5 levels, gamma): +32/255 with the mask, -8/255 without it; any change resets it.
float Overlay(int2 p, float3 g)
{
    if (gU2.z == 0) return 0.0;
    const float m = gHud.Load(int3(p, 0));
    if (gU2.y == 0) return 0.0;
    const float3 d = abs(g - FromWork(gWorkPrev.Load(int3(p, 0)).rgb)) * 255.0;
    if (max(d.r, max(d.g, d.b)) >= 1.5) return 0.0;
    const float e = round(gOvlPrev.Load(int3(p, 0)) * 255.0);
    return (m >= 0.5 ? min(e + 32.0, 255.0) : max(e - 8.0, 0.0)) / 255.0;
}

PsOut main(VsOut i)
{
    const int2 p = int2(i.pos.xy);
    const int2 hi = int2(gIn.xy) - 1;
    // 5x5 binomial as 3 x 3 bilinear fetches at offsets -1.2, 0, +1.2 per axis, weights 5, 6, 5 (a fetch at -1.2
    // weighs texels -2 and -1 as 1 : 4; luma is linear in RGB, so filtering before LumaOf is exact up to the sampler's
    // weight precision; clamp-to-edge = the former clamped Loads). Stage 3 cost cut: 25 Loads -> 9 fetches.
    static const float kO[3] = {-1.2, 0.0, 1.2};
    static const float kW[3] = {5.0, 6.0, 5.0};
    float l = 0;
    [unroll] for (int k = 0; k < 9; ++k)
        l += kW[k % 3] * kW[k / 3] * LumaOf(gCurr.SampleLevel(gLinear, (i.pos.xy + float2(kO[k % 3], kO[k / 3])) * gIn.zw, 0).rgb);
    const float3 g = gCurr.Load(int3(p, 0)).rgb;
    PsOut o;
    o.work = float4(ToWork(g), l / 256.0);
    o.ovl = Overlay(p, g);
    return o;
}
