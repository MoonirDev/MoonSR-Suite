// MoonSR temporal upscaler seed pass (output resolution): the single-frame reconstruction (MoonSR Spatial, see
// alsr_seed.hlsli) for every pixel. Used by RenderSingle (generated frames); Render evaluates the seed inside
// alsr_seedblend only where it is needed. Owner: upscaling-engineer.
// t0 = frame (gamma), b1 = SeedConstants. Output: gamma RGBA8.
#include "upscale/alsr_seed.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VsOut i) : SV_Target
{
    return float4(saturate(AlsrSeed(uint2(i.pos.xy))), 1.0);
}
