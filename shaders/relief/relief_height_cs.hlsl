// Smart parallax analysis, height pass (full game resolution, no group shared memory): relief height H and its temporal
// history. Owner: upscaling-engineer. Research note 1.2 steps 2-4, 3.1, 3.6:
//   B, D      = guided-filter base and detail: B = mean_a * L + mean_b (half-size coefficients, bilinear), D = L - B
//   H_raw     = tanh(D / (k * sigma + floor))      contrast-normalised height, "dark is deep" (sigma from the gate pass)
//   history   = lerp(H_prev(reprojected), H_raw, alpha); alpha = 1 on reset / scene cut / changed pixels
// The protection gates themselves live in the half-size gate texture G (relief_gate_cs.hlsl), read by Apply.
#include "relief/relief_common.hlsli"

Texture2D<float2>      abTex       : register(t0); // half-size guided-filter coefficients (mean_a, mean_b)
Texture2D<float>       curL        : register(t1);
Texture2D<float>       prevH       : register(t3);
Texture2DArray<float4> flowTex     : register(t4); // MotionFieldView::flow (optional)
Buffer<uint>           motionStats : register(t5); // MotionFieldView::stats (optional)
ByteAddressBuffer      state       : register(t6); // frame state (relief_frame.hlsli)
Texture2D<float4>      gateTex     : register(t7); // half-size gates (hud, edge * tex, changed, SigmaEnc(sigma))
Texture2D<float>       edgeTex     : register(t8);
SamplerState           linClamp    : register(s0);

RWTexture2D<float>  outH     : register(u0);

void HeightPixel(int2 p, bool cut)
{
    if (any(p >= int2(cSize))) return;
    const float2 uv = (float2(p) + 0.5) / float2(cSize);

    const float lNow = LDec(curL.Load(int3(p, 0)));
    const float2 ab = abTex.SampleLevel(linClamp, uv, 0);
    const float d = lNow - (ab.x * lNow + ab.y);
    const float4 gate = gateTex.SampleLevel(linClamp, uv, 0);
    const float sigma = SigmaDec(gate.w);
    const float edge = edgeTex.SampleLevel(linClamp, uv, 0);
    const float keep = smoothstep(0.08, 0.95, edge);
    const float hRaw = keep * ReliefTanh(d / (cSigmaK * sigma + cSigmaFloor));

    const bool pixelChanged = gate.z > 0.25; // change exceeded the gate pass's luminance-dependent noise floor

    // Temporal history.
    float alpha = 1.0;
    float2 prevPos = float2(p) + 0.5;
    if (!cut) {
        if (cFlags & RELIEF_F_MOTION) {
            const int2 cell = min(int2((float2(p) + 0.5) * cCellInv), int2(cCells) - 1);
            const float4 f = flowTex.Load(int4(cell, 1, 0)); // slice 1: newest -> previous
            prevPos += f.xy;
            alpha = lerp(1.0, cHistoryAlpha, saturate(f.z));
        } else if (!pixelChanged) {
            alpha = cHistoryAlpha;
        }
        if (any(prevPos < 0.0) || any(prevPos > float2(cSize))) alpha = 1.0;
    }
    float h = hRaw;
    alpha = lerp(1.0, alpha, keep);
    if (alpha < 1.0) h = lerp(prevH.SampleLevel(linClamp, prevPos / float2(cSize), 0), hRaw, alpha);
    outH[p] = h;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    // The gate pass finishes the scene-cut decision before this dispatch. Share it across adjacent pixels.
    bool cut = (cFlags & RELIEF_F_RESET) != 0 || asfloat(state.Load(RST_CUT)) > 0.5;
    if (cFlags & RELIEF_F_MOTION_CUT) cut = cut || motionStats.Load(cSceneCutIndex) != 0;

    const int2 origin = int2(dtid.xy) * 2;
    HeightPixel(origin, cut);
    HeightPixel(origin + int2(1, 0), cut);
    HeightPixel(origin + int2(0, 1), cut);
    HeightPixel(origin + int2(1, 1), cut);
}
