// SPDX-License-Identifier: GPL-3.0-only
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer ColourDirection : register(b0) { uint EncodeSrgb; }

float ToLinear(float x) {
    return x <= 0.04045 ? x / 12.92 : pow(max(0.0, (x + 0.055) / 1.055), 2.4);
}

float ToSrgb(float x) {
    return x <= 0.0031308 ? x * 12.92 : 1.055 * pow(max(0.0, x), 1.0 / 2.4) - 0.055;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    Destination.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float4 value = Source.Load(int3(id.xy, 0));
    if (EncodeSrgb)
        value.rgb = float3(ToSrgb(value.r), ToSrgb(value.g), ToSrgb(value.b));
    else
        value.rgb = float3(ToLinear(value.r), ToLinear(value.g), ToLinear(value.b));
    Destination[id.xy] = value;
}
