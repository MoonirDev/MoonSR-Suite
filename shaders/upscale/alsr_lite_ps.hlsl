#include "upscale/alsr_lite.hlsli"

// Screen-space stabilisation for a GPU tier without temporal motion estimation.
// Only a narrow band of small changes can retain the old sample. Changes outside
// that band take the new reconstruction immediately.
struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> gCurrent : register(t0);
Texture2D<float4> gPrevious : register(t1);

float4 main(VsOut i) : SV_Target
{
    const int3 p = int3(int2(i.pos.xy), 0);
    const float3 current = gCurrent.Load(p).rgb;
    const float4 previous = gPrevious.Load(p);
    return float4(StabilizeLite(current, previous), 1.0);
}
