// Frame (BGRA8, gamma) -> pyramid level 0 luma at flow resolution, area-averaged.
//   gSrcSize  frame size           gDstSize  level-0 size
//   gU0.x     samples per axis n (ceil of frame/level ratio, 1..4)
// Output values are 0..255. Each output pixel averages an n x n grid of bilinear taps spread over its footprint,
// which is an exact box filter whenever the ratio is an integer (taps land on texel centres).
#include "framegen/fg_common.hlsli"

Texture2D<float4> gFrame : register(t0);
SamplerState gLinear : register(s0);
RWTexture2D<float> gOut : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(int2(id.xy) >= gDstSize)) return;
    const float2 ratio = float2(gSrcSize) / float2(gDstSize);
    const float2 invSrc = 1.0 / float2(gSrcSize);
    const uint n = gU0.x;
    float sum = 0.0;
    [loop] for (uint j = 0; j < n; ++j) {
        [loop] for (uint i = 0; i < n; ++i) {
            const float2 f = (float2(id.xy) + (float2(i, j) + 0.5) / float(n)) * ratio;
            sum += Luma(gFrame.SampleLevel(gLinear, f * invSrc, 0).rgb);
        }
    }
    gOut[id.xy] = sum * (255.0 / float(n * n));
}
