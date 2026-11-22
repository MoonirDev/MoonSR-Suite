// Smart parallax (screen-space relief), groundwork: exact pass-through. Owner: upscaling-engineer.
// Drawn with common/fullscreen_vs.hlsl over a viewport of exactly the source size, so every destination pixel reads the
// source texel at the same integer position (no filtering, bit-exact for equal formats). The relief algorithm (M1+)
// replaces this shader; until then it only runs when the source cannot be copied with CopySubresourceRegion (different
// formats), so the plumbing, state handling and GPU timing of the final stage are real.
Texture2D<float4> srcTex : register(t0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(VsOut i) : SV_Target
{
    return srcTex.Load(int3(int2(i.pos.xy), 0));
}
