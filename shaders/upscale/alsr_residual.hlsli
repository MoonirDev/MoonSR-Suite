// Evaluate the existing back-projection measurement model at one source sample.
#ifndef MOONSR_ALSR_RESIDUAL_HLSLI
#define MOONSR_ALSR_RESIDUAL_HLSLI

float3 MeasurementResidual(int2 sampleIndex)
{
    // Overlay boundary ring: no residual, so the back-projection never carries information across the layer boundary.
    if (gLayer.Load(int3(sampleIndex, 0)).y > 0.5) return 0;
    const float3 c = gWork.Load(int3(sampleIndex, 0)).rgb;
    const float2 q = (float2(sampleIndex) + 0.5) * gScale.xy;
#ifdef ALSR_ADAPTIVE_FOOTPRINT
    // Model a confirmed point sample at its centre before interpolating the residual.
    [branch] if ((gU1.w & 32u) == 0 && gSampling.Load(int3(0, 0, 0)).x == 2.0) {
        return c - gHw.SampleLevel(gLinear, q * gOut.zw, 0).rgb;
    }
#endif
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
    return c - pred;
}
#endif
