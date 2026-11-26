#ifndef MOONSR_ALSR_LITE_HLSLI
#define MOONSR_ALSR_LITE_HLSLI

float3 StabilizeLite(float3 current, float4 previous)
{
    const float3 difference = abs(current - previous.rgb);
    const float span = max(difference.r, max(difference.g, difference.b));
    const float keep = 0.45 * (1.0 - smoothstep(1.0 / 255.0, 5.0 / 255.0, span)) * previous.a;
    return lerp(current, previous.rgb, keep);
}

#endif
