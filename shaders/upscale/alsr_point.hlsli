// Reconstruct the current point sample and blend it where motion supports a new sample phase.
#ifndef MOONSR_ALSR_POINT_HLSLI
#define MOONSR_ALSR_POINT_HLSLI

float4 AccumulatePoint(float4 prior, float2 p)
{
    const int2 hi = int2(gIn.xy) - 1;
    const int2 nearest = clamp(int2(p), 0, hi);
    const float3 current = gWork.Load(int3(nearest, 0)).rgb;
#if defined(ALSR_POINT_RESIDUAL)
    // Measure the current point against the prediction at the same phase, rather
    // than comparing it to the colour at this output pixel.
    const float2 predictionUv = (float2(nearest) + 0.5) * gScale.xy * gOut.zw;
    const float3 difference = current - gHw.SampleLevel(gLinear, predictionUv, 0).rgb;
#endif
    if (prior.a < 0.0625 || gLayer.Load(int3(nearest, 0)).y > 0.5)
        return float4(prior.rgb, 1.0);

    const float4 motion = gMotion.Load(int3(nearest, 0));
    if (all(motion.xy == 0.0)) {
        const float3 previous = gWorkPrev.Load(int3(nearest, 0)).rgb;
        const float changed = smoothstep(gP2.x, gP2.y, abs(LumaOf(current - previous)));
#ifdef ALSR_POINT_RESIDUAL
        return float4(saturate(prior.rgb + difference * changed), lerp(prior.a, 1.0, changed));
#else
        return float4(lerp(prior.rgb, current, changed), lerp(prior.a, 1.0, changed));
#endif
    }

    // A local luma gradient steers a narrow kernel across an edge and a broader
    // one along it. Spread reflects how far a source sample is from this output location.
    const float2 gradient = float2(
        LumaOf(gWork.Load(int3(min(nearest + int2(1, 0), hi), 0)).rgb -
               gWork.Load(int3(max(nearest - int2(1, 0), 0), 0)).rgb),
        LumaOf(gWork.Load(int3(min(nearest + int2(0, 1), hi), 0)).rgb -
               gWork.Load(int3(max(nearest - int2(0, 1), 0), 0)).rgb));
    const float gradient2 = dot(gradient, gradient);
    const float2 offset = p - (float2(nearest) + 0.5);
    const float across = dot(gradient, offset);
    const float radius2 = 5.0 * dot(offset, offset) + 13.0 * across * across / (gradient2 + 0.01);
    const float evidence = exp2(-radius2) * lerp(0.35, 1.0, saturate(motion.z));
    const float historyWeight = min(prior.a, max(gP0.w - evidence, 0.0));
    const float total = historyWeight + evidence;
#ifdef ALSR_POINT_RESIDUAL
    const float gain = evidence / max(total, 1e-4);
    return float4(saturate(prior.rgb + difference * gain), total);
#else
    return float4(lerp(prior.rgb, current, evidence / max(total, 1e-4)), total);
#endif
}

#ifdef ALSR_GUARDED_POINT
float4 BlendPoint(float4 area, float4 prior, float2 pos, int2 nearest, bool usePoint)
{
    float4 combined = area;
    [branch] if (usePoint && prior.a > 1.0) {
        const float4 local = gMotion.Load(int3(nearest, 0));
        const int2 hiIndex = int2(gIn.xy) - 1;
        const float2 vx = gMotion.Load(int3(min(nearest + int2(1, 0), hiIndex), 0)).xy;
        const float2 vy = gMotion.Load(int3(min(nearest + int2(0, 1), hiIndex), 0)).xy;
        const float variance = max(length(local.xy - vx), length(local.xy - vy));
        const float confidence = smoothstep(0.30, 0.65, local.z);
        const float coherence = 1.0 - smoothstep(0.15, 0.75, variance);
        const float match = 1.0 - smoothstep(0.018, 0.10, local.w);
        const float moving = smoothstep(0.05, 0.20, length(local.xy));
        const float support = confidence * coherence * match * moving * smoothstep(1.0, 2.0, prior.a);
        [branch] if (support > 0.01) {
            const float4 estimate = AccumulatePoint(prior, pos);
#ifdef ALSR_FIXED_POINT_COUNT
            // Vary the colour correction independently of the age of the area history.
            combined.rgb = lerp(area.rgb, estimate.rgb, support);
#else
            combined = lerp(area, estimate, support);
#endif
        }
    }
    return combined;
}
#endif
#endif
