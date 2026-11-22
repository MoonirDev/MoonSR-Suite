// Frame counter overlay vertex shader. One instance per glyph quad, 4 vertices as a TRIANGLESTRIP (SV_VertexID).
// Per-instance data: rect (x0, y0, x1, y1) in target pixels (y down), uvRect (u0, v0, u1, v1) in atlas texels.
// C++ mirror of the constant buffer: `OverlayConstants` in src/overlay/stats_overlay.cpp.
cbuffer OverlayConstants : register(b0)
{
    float2 invTarget;   // 1 / target size in pixels
    float2 invAtlas;    // 1 / atlas size in texels
    float  outlineTexels; // outline radius in texels (== pixels, the atlas is drawn 1:1)
    float3 pad;
};

struct VsIn
{
    float4 rect   : RECT;
    float4 uvRect : UVRECT;
    uint   id     : SV_VertexID;
};

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut main(VsIn i)
{
    float2 t = float2(i.id & 1, i.id >> 1); // (0,0) (1,0) (0,1) (1,1)
    float2 px = lerp(i.rect.xy, i.rect.zw, t);
    VsOut o;
    o.pos = float4(px.x * invTarget.x * 2.0 - 1.0, 1.0 - px.y * invTarget.y * 2.0, 0.0, 1.0);
    o.uv = lerp(i.uvRect.xy, i.uvRect.zw, t) * invAtlas;
    return o;
}
