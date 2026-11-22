// Cursor overlay pixel shader (dual-source blending, one pass for every cursor kind).
//
// Each cursor texel is either "paint" (premultiplied colour c, coverage a) or "XOR" (colour s, white = invert),
// never both. On a destination value d the desired result is
//     paint: c + d * (1 - a)          XOR: s * (1 - d) + d * (1 - s)   (exact XOR for 0/1 channels)
// Both are of the form  src0 * (1 - d) + d * src1  with
//     src0 = c + s,   src1 = 1 - a + c - s
// (paint: s = 0; XOR: c = a = 0), and src0, src1 stay in [0, 1]. The blend state is
//     SrcBlend = INV_DEST_COLOR, DestBlend = SRC1_COLOR, BlendOp = ADD.
// Because src0 and src1 are linear in (c, a, s), bilinear filtering of the two textures gives exactly the filtered
// composite, including texels where paint and XOR pixels meet (two separate passes would not commute there).
// Alpha: out.a = a + d.a * (1 - a) (SrcBlendAlpha = ONE, DestBlendAlpha = INV_SRC_ALPHA).
Texture2D<float4> paintTex : register(t0); // premultiplied BGRA (c, a)
Texture2D<float4> xorTex   : register(t1); // XOR colour s in rgb, alpha unused
SamplerState linearClamp : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

struct PsOut
{
    float4 src0 : SV_Target0;
    float4 src1 : SV_Target1;
};

PsOut main(VsOut i)
{
    float4 p = paintTex.SampleLevel(linearClamp, i.uv, 0.0);
    float3 s = xorTex.SampleLevel(linearClamp, i.uv, 0.0).rgb;
    PsOut o;
    o.src0 = float4(saturate(p.rgb + s), p.a);
    o.src1 = float4(saturate(1.0 - p.a + p.rgb - s), 1.0 - p.a);
    return o;
}
