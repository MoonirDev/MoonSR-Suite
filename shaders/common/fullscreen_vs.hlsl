// Fullscreen triangle. Draw(3, 0) with no vertex buffer. The viewport selects the destination rectangle.
struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut main(uint id : SV_VertexID)
{
    VsOut o;
    float2 t = float2((id << 1) & 2, id & 2); // (0,0) (2,0) (0,2)
    o.uv = t;
    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
