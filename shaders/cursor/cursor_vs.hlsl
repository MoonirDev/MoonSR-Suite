// Cursor overlay quad. Draw(4, 0) as a TRIANGLESTRIP with no vertex buffer / input layout.
// The quad's corners come from a constant buffer in normalised device coordinates, so the cursor can be placed
// anywhere on (and partly outside) the render target with the viewport covering the whole target.
// C++ mirror: `CursorConstants` in src/cursor/cursor_overlay.cpp.
cbuffer CursorConstants : register(b0)
{
    float4 ndcRect; // x0, y0 (top-left), x1, y1 (bottom-right) in NDC (y up)
};

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut main(uint id : SV_VertexID)
{
    VsOut o;
    float2 t = float2(id & 1, id >> 1); // (0,0) (1,0) (0,1) (1,1)
    o.uv = t;
    o.pos = float4(lerp(ndcRect.xy, ndcRect.zw, t), 0.0, 1.0);
    return o;
}
