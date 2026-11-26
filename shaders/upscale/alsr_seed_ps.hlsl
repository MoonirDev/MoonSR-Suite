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

#ifdef ALSR_LITE_SEED
#include "upscale/alsr_lite.hlsli"
Texture2D<float4> gPrevious : register(t1);
struct LiteOut
{
    float4 display : SV_Target0;
    float4 seed : SV_Target1;
};

LiteOut main(VsOut i)
{
    const int3 p = int3(int2(i.pos.xy), 0);
    const float3 current = saturate(AlsrSeed(uint2(p.xy)));
    LiteOut output;
    output.display = float4(StabilizeLite(current, gPrevious.Load(p)), 1.0);
    output.seed = float4(current, 1.0);
    return output;
}
#else
float4 main(VsOut i) : SV_Target
{
    return float4(saturate(AlsrSeed(uint2(i.pos.xy))), 1.0);
}
#endif
