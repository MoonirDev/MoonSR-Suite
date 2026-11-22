// Moon ALSR pass 8 (source resolution, end of Render): registration reference for the NEXT frame.
// Owner: upscaling-engineer. Output (R16F): luma of the new history (gamma-encoded, BT.709 weights) reduced to the
// source grid and blurred with the same 5x5 binomial kernel as pass 0:
//   ref(x) = sum_k b_k Y(H at the output position of x + o_k),   H fetched bilinearly at the source pixel centre
//   mapped to the output (at 2x this is exactly the 2x2 area average of the output pixels the source pixel covers).
// Two passes since Stage 3 (same result; 25 x (bilinear fetch + 3 pow) per pixel before, 0.11 ms of 1.86 ms on the RTX
// 5060 at 720p -> 1080p):
//   main_y (ALSR_REF_Y): Y(x) = luma(FromWork(H bilinear at x * out/src)) -> R32F, one fetch per source pixel;
//   main            : the 5x5 binomial as 3 x 3 bilinear fetches of Y at offsets -1.2, 0, +1.2 per axis with weights
//                     5, 6, 5 (a bilinear fetch at -1.2 weighs texels -2 and -1 as 1 : 4, exactly the binomial pair;
//                     clamp-to-edge equals the former per-tap position clamp).
// Measured in Stage 3: interpolating the gamma-encoded history copy instead of the linear history, or a 3 x 3 tap
// approximation of the blur, cost up to 0.65 dB on aliased 2x pans (worse registration); this form is exact.
// Why a reference from the history at all: matching two ALIASED frames measures the motion of their aliasing
// patterns, which differs systematically from the scene motion (thin features at a regular pitch alias coherently;
// measured with tbench --motion-diag on the aliased slow pan: mean estimate (0.07, 0.38) for a true (0.31, 0.22)
// source px). The accumulated history is largely free of that aliasing, so registering the new frame against it turns
// the new frame's aliasing into uncorrelated noise, which the large refinement window averages out.
#include "upscale/alsr_common.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

#ifdef ALSR_REF_Y
Texture2D<float4> gHist : register(t0); // the history (working space, output res)

float main(VsOut i) : SV_Target
{
    return LumaOf(FromWork(gHist.SampleLevel(gLinear, i.pos.xy * gScale.xy * gOut.zw, 0).rgb));
}
#else
Texture2D<float> gY : register(t0); // main_y output (source res)

float main(VsOut i) : SV_Target
{
    static const float kO[3] = {-1.2, 0.0, 1.2};
    static const float kW[3] = {5.0, 6.0, 5.0};
    float l = 0;
    [unroll] for (int k = 0; k < 9; ++k)
        l += kW[k % 3] * kW[k / 3] * gY.SampleLevel(gLinear, (i.pos.xy + float2(kO[k % 3], kO[k / 3])) * gIn.zw, 0);
    return l / 256.0;
}
#endif
