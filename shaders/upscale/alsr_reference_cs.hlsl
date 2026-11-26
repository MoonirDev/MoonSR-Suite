// Build the source-resolution registration reference in one dispatch.
// Each group down-samples a tile (including the two-pixel blur halo) into group-shared memory,
// then applies the same separable 5x5 binomial as the two-pass implementation.
#include "upscale/alsr_common.hlsli"

Texture2D<float4> gHist : register(t0);
RWTexture2D<float> gRef : register(u0);

static const uint kWidth = 16;
static const uint kHeight = 8;
static const uint kStride = kWidth + 4;
static const uint kRows = kHeight + 4;
static const uint kThreads = kWidth * kHeight;
groupshared float gLuma[kStride * kRows];
groupshared float gHorizontal[kWidth * kRows];

[numthreads(16, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 tid : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const int2 origin = int2(group.xy * uint2(kWidth, kHeight));
    const int2 last = int2(gIn.xy) - 1;
    for (uint slot = lane; slot < kStride * kRows; slot += kThreads) {
        const int2 sampleIndex = clamp(origin + int2(slot % kStride, slot / kStride) - 2, 0, last);
        const float2 p = (float2(sampleIndex) + 0.5) * gScale.xy * gOut.zw;
        gLuma[slot] = LumaOf(FromWork(gHist.SampleLevel(gLinear, p, 0).rgb));
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint slotX = lane; slotX < kWidth * kRows; slotX += kThreads) {
        const uint first = (slotX / kWidth) * kStride + slotX % kWidth;
        gHorizontal[slotX] = (gLuma[first] + gLuma[first + 4]) +
            4.0 * (gLuma[first + 1] + gLuma[first + 3]) + 6.0 * gLuma[first + 2];
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 pOut = origin + int2(tid.xy);
    if (any(pOut > last)) return;
    const uint top = tid.y * kWidth + tid.x;
    const float blurred = (gHorizontal[top] + gHorizontal[top + 4 * kWidth]) +
        4.0 * (gHorizontal[top + kWidth] + gHorizontal[top + 3 * kWidth]) +
        6.0 * gHorizontal[top + 2 * kWidth];
    // The portable UAV format is R32F. Match the reference texture's half precision on write.
    gRef[pOut] = f16tof32(f32tof16(blurred * (1.0 / 256.0)));
}
