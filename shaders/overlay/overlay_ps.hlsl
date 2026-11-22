// Frame counter overlay pixel shader. The atlas holds glyph coverage (R8). Output is premultiplied:
// white glyph over a dark outline (the max of 8 taps at the outline radius), blended with
// SrcBlend = ONE, DestBlend = INV_SRC_ALPHA. The atlas is drawn 1:1 with point sampling and every cell carries
// enough empty padding that the outline taps never reach the neighbouring glyph's ink.
cbuffer OverlayConstants : register(b0)
{
    float2 invTarget;
    float2 invAtlas;
    float  outlineTexels;
    float3 pad;
};

Texture2D<float> atlas : register(t0);
SamplerState pointClamp : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(VsOut i) : SV_Target
{
    float c = atlas.SampleLevel(pointClamp, i.uv, 0.0);
    float2 r = outlineTexels * invAtlas;
    float o = 0.0;
    o = max(o, atlas.SampleLevel(pointClamp, i.uv + float2(-r.x, 0.0), 0.0));
    o = max(o, atlas.SampleLevel(pointClamp, i.uv + float2( r.x, 0.0), 0.0));
    o = max(o, atlas.SampleLevel(pointClamp, i.uv + float2(0.0, -r.y), 0.0));
    o = max(o, atlas.SampleLevel(pointClamp, i.uv + float2(0.0,  r.y), 0.0));
    o = max(o, atlas.SampleLevel(pointClamp, i.uv + float2(-r.x, -r.y), 0.0));
    o = max(o, atlas.SampleLevel(pointClamp, i.uv + float2( r.x, -r.y), 0.0));
    o = max(o, atlas.SampleLevel(pointClamp, i.uv + float2(-r.x,  r.y), 0.0));
    o = max(o, atlas.SampleLevel(pointClamp, i.uv + float2( r.x,  r.y), 0.0));
    float a = c + (1.0 - c) * o * 0.85;
    return float4(c, c, c, a);
}
