// Moon ALSR pass 4 (source resolution): measurement residual. Owner: upscaling-engineer.
// Output (RGBA16F): rgb = C_k - P(s_k), a = 0, for every source sample k at s_k (centre of source pixel k):
//   C_k   = the current captured sample (working space),
//   P(s)  = the prediction of that sample from the reprojected history Hw (pass 3) under an AREA sampling model: the
//           mean of Hw over the sample's footprint (4 bilinear taps at +-footprint/4; exactly the 2x2 box at 2x).
//           Measured against the point model (Catmull-Rom of Hw at s, development flag 32) on tbench static / slow pan,
//           2x: prefiltered input +1.8 / +0.4 dB, aliased input -0.2 / -0.1 dB. On a static prefiltered scene the area
//           model makes the iteration a deconvolution of the footprint box: it sharpens towards the ground truth of the
//           synthetic model, which is a property of the assumed sampling model and not new information.
// Where the history predicts the new samples, the residual is zero and the history is kept as it is (no blur from
// re-interpolating the new frame). Where it does not (new content, sub-pixel phases never seen before, wrong history),
// the residual carries exactly the new information. See alsr_update_ps.hlsl.
#include "upscale/alsr_common.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> gHw : register(t0);   // output res
Texture2D<float4> gWork : register(t1); // source res
Texture2D<float2> gLayer : register(t2); // overlay layer / boundary ring (alsr_motion MRT1)

float4 main(VsOut i) : SV_Target
{
    // Overlay boundary ring: no residual, so the back-projection never carries information across the layer boundary.
    if (gLayer.Load(int3(int2(i.pos.xy), 0)).y > 0.5) return float4(0, 0, 0, 0);
    const float3 c = gWork.Load(int3(int2(i.pos.xy), 0)).rgb;
    const float2 q = i.pos.xy * gScale.xy;
    float3 pred;
    if ((gU1.w & 32u) == 0) {
        // Area model (default): mean of 4 bilinear taps at +-1/4 of the footprint (exactly the 2x2 box at 2x).
        const float2 d = 0.25 * gScale.xy;
        pred = 0.25 * (gHw.SampleLevel(gLinear, (q + float2(-d.x, -d.y)) * gOut.zw, 0).rgb + gHw.SampleLevel(gLinear, (q + float2(d.x, -d.y)) * gOut.zw, 0).rgb +
                       gHw.SampleLevel(gLinear, (q + float2(-d.x, d.y)) * gOut.zw, 0).rgb + gHw.SampleLevel(gLinear, (q + float2(d.x, d.y)) * gOut.zw, 0).rgb);
    } else {
        // Development flag 32: point model, Catmull-Rom of Hw at the sample position.
        pred = SampleCR(gHw, q, gOut.zw, false).rgb;
    }
    return float4(c - pred, 0.0);
}
