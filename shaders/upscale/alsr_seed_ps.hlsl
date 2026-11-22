// Moon ALSR seed pass (output resolution): the single-frame reconstruction (MoonSR Spatial + learned filter bank, see
// alsr_seed.hlsli) for every pixel. Used by RenderSingle (generated frames) and the LearnedBlend debug view; Render
// evaluates the seed inside alsr_reproject only where it is needed. Owner: upscaling-engineer.
// t0 = frame (gamma), t1 = filter table, b1 = SeedConstants. Output: gamma RGBA8.
// Debug (gSeedMisc.x == 1): grey generic luma with the bucket's learned blend weight alpha in red.
#include "upscale/alsr_seed.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VsOut i) : SV_Target
{
    float alpha;
    float3 o = saturate(AlsrSeed(uint2(i.pos.xy), alpha));
    if (gSeedMisc.x == 1) {
        const float y = 0.6 * Luma(o);
        o = float3(max(y, alpha), y * (1.0 - alpha), y * (1.0 - alpha));
    }
    return float4(o, 1.0);
}
