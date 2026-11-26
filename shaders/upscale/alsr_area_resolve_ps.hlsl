#include "upscale/alsr_common.hlsli"

Texture2D<float4> gHistory : register(t0);

float4 main(float4 pos : SV_Position) : SV_Target
{
    return float4(FromWork(gHistory.Load(int3(int2(pos.xy), 0)).rgb), 1.0);
}
