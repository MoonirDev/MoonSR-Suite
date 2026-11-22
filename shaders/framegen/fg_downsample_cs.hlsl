// Pyramid level l -> level l+1 with a separable [1 3 3 1] / 8 low-pass (4x4 taps). Destination size is (src + 1) / 2
// so that position u in level l+1 corresponds exactly to 2u in level l; reads are edge-clamped.
// A 2x2 box was used first; it leaves fine periodic patterns (fences, grilles) aliased into moire at the coarse
// levels, and moire moves with a different apparent velocity than the pattern, so the coarse search locked onto it.
// The wider kernel attenuates those frequencies much more strongly.
//   gSrcSize  level l size     gDstSize  level l+1 size
#include "framegen/fg_common.hlsli"

Texture2D<float> gSrc : register(t0);
RWTexture2D<float> gOut : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(int2(id.xy) >= gDstSize)) return;
    const int2 p = int2(id.xy) * 2 - 1;
    const float w[4] = {1.0, 3.0, 3.0, 1.0};
    float sum = 0.0;
    [unroll] for (int y = 0; y < 4; ++y) {
        float row = 0.0;
        [unroll] for (int x = 0; x < 4; ++x) row += w[x] * gSrc.Load(int3(ClampI(p + int2(x, y), gSrcSize), 0));
        sum += w[y] * row;
    }
    gOut[id.xy] = sum / 64.0;
}
