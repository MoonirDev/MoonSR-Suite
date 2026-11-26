// Moon ALSR: shared constants and helpers. Owner: upscaling-engineer.
// C++ mirror: `AlsrConstants` in src/upscale/alsr_upscaler.cpp. Never change one without the other.
//
// Coordinate conventions (all passes): pixel centres at integer + 0.5. "Source" / "frame" = the captured frame at game
// resolution (the generator's frame size); "output" = the reconstruction at outputSize. A source position x maps to the
// output position x * gScale.xy and back with gScale.zw.
#ifndef MOONSR_ALSR_COMMON_HLSLI
#define MOONSR_ALSR_COMMON_HLSLI

cbuffer AlsrConstants : register(b0)
{
    float4 gIn;    // source w, h, 1/w, 1/h
    float4 gOut;   // output w, h, 1/w, 1/h
    float4 gScale; // output/source (x, y), source/output (x, y)
    float4 gCell;  // motion cells w, h, frame px per cell, cells per frame px
    float4 gP0;    // sharpness, minimum gain, clip reference (static), maximum accumulated count
    float4 gP1;    // confidence ramp lo, hi; match error ramp lo, hi
    float4 gP2;    // gain boost residual ramp lo, hi; clip at full motion (box shrink = gP2.z / gP0.z); motion (src px) of it
    float4 gP3;    // visibility tau0 (frame px), tauK; smooth-candidate spread threshold (frame px); motion agreement (src px)
    uint4 gU0;     // history valid, overlay layer enabled (persistent mask, alsr_prep), linear working space, debug view
    uint4 gU1;     // scene-cut stats index, stats valid, input is gamma (final pass), variant flags
    float4 gP4;    // velocity filter: minimum measurement weight, full-weight deviation lo, hi (frame px); hull margin
    uint4 gU2;     // previous refined cells valid, previous working copy valid, overlay mask bound, 0
};

SamplerState gLinear : register(s0);
SamplerState gPoint : register(s1);

static const float3 kLumaW = float3(0.2126, 0.7152, 0.0722);

float LumaOf(float3 c) { return dot(c, kLumaW); }

// Exact sRGB transfer functions (IEC 61966-2-1).
float3 SrgbToLinear(float3 c)
{
    c = saturate(c);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// Gamma-encoded value -> working space and back (gU0.z selects linear light).
float3 ToWork(float3 g) { return gU0.z != 0 ? SrgbToLinear(g) : g; }
float3 FromWork(float3 w) { return gU0.z != 0 ? LinearToSrgb(w) : saturate(w); }

float3 RgbToYCoCg(float3 c)
{
    const float y = 0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
    const float co = 0.5 * c.r - 0.5 * c.b;
    const float cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
    return float3(y, co, cg);
}
float3 YCoCgToRgb(float3 c)
{
    const float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

void CubicWeights(float2 f, float a, out float2 w0, out float2 w1, out float2 w2, out float2 w3)
{
    // Keys kernel: |x| <= 1: (a+2)|x|^3 - (a+3)|x|^2 + 1;  1 < |x| < 2: a|x|^3 - 5a|x|^2 + 8a|x| - 4a.
    const float2 g = 1.0 - f;
    w0 = a * (1.0 + f) * (1.0 + f) * (1.0 + f) - 5.0 * a * (1.0 + f) * (1.0 + f) + 8.0 * a * (1.0 + f) - 4.0 * a;
    w1 = (a + 2.0) * f * f * f - (a + 3.0) * f * f + 1.0;
    w2 = (a + 2.0) * g * g * g - (a + 3.0) * g * g + 1.0;
    w3 = a * (2.0 - f) * (2.0 - f) * (2.0 - f) - 5.0 * a * (2.0 - f) * (2.0 - f) + 8.0 * a * (2.0 - f) - 4.0 * a;
}

// Cubic convolution (Keys; a = -0.5 is Catmull-Rom, a = -0.75 is sharper) at position pos (texels of `tex`, centres at
// +0.5), 9 bilinear taps:
// per axis the weights w0..w3 are applied as w0 at texel -1, (w1 + w2) at a fractional position between texels 0 and 1
// (one bilinear fetch with ratio w2 / (w1 + w2); w1 + w2 >= 1 for every phase, so the ratio is well defined), w3 at
// texel +2. Positions outside the texture clamp to the edge (sampler). Optionally clamps the result to the range of
// the 2x2 nearest texels (anti-ringing).
float4 SampleCubicA(Texture2D<float4> tex, float2 pos, float2 invSize, bool deringing, float a)
{
    const float2 p1 = floor(pos - 0.5) + 0.5;
    const float2 f = pos - p1;
    float2 w0, w1, w2, w3;
    CubicWeights(f, a, w0, w1, w2, w3);
    const float2 w12 = w1 + w2;
    const float2 q0 = (p1 - 1.0) * invSize;
    const float2 q12 = (p1 + w2 / w12) * invSize;
    const float2 q3 = (p1 + 2.0) * invSize;
    float4 c = 0;
    c += (tex.SampleLevel(gLinear, float2(q0.x, q0.y), 0) * w0.x + tex.SampleLevel(gLinear, float2(q12.x, q0.y), 0) * w12.x +
          tex.SampleLevel(gLinear, float2(q3.x, q0.y), 0) * w3.x) * w0.y;
    c += (tex.SampleLevel(gLinear, float2(q0.x, q12.y), 0) * w0.x + tex.SampleLevel(gLinear, float2(q12.x, q12.y), 0) * w12.x +
          tex.SampleLevel(gLinear, float2(q3.x, q12.y), 0) * w3.x) * w12.y;
    c += (tex.SampleLevel(gLinear, float2(q0.x, q3.y), 0) * w0.x + tex.SampleLevel(gLinear, float2(q12.x, q3.y), 0) * w12.x +
          tex.SampleLevel(gLinear, float2(q3.x, q3.y), 0) * w3.x) * w3.y;
    if (deringing) {
        // The 2x2 texels around pos: one Gather per channel would need a known format; four point fetches are simple.
        const float2 b = p1 * invSize;
        const float4 t00 = tex.SampleLevel(gPoint, b, 0);
        const float4 t10 = tex.SampleLevel(gPoint, b + float2(invSize.x, 0), 0);
        const float4 t01 = tex.SampleLevel(gPoint, b + float2(0, invSize.y), 0);
        const float4 t11 = tex.SampleLevel(gPoint, b + invSize, 0);
        c = clamp(c, min(min(t00, t10), min(t01, t11)), max(max(t00, t10), max(t01, t11)));
    }
    return c;
}

float4 SampleCR(Texture2D<float4> tex, float2 pos, float2 invSize, bool deringing) { return SampleCubicA(tex, pos, invSize, deringing, -0.5); }

// Persistent overlay evidence (alsr_prep MRT1, R8): +32/255 per frame in which the generator's static mask says
// "overlay" (>= 0.5) and the pixel is unchanged in place, -8/255 per unchanged frame without the mask, 0 as soon as
// the pixel changes. The overlay strength used by every other pass ramps from 0 at 3 such frames to 1 at 5: the
// generator's per-pair mask has isolated false positives inside slowly moving aliased texture (a point-sampled pixel
// can stay unchanged for a frame or two while the scene moves sub-pixel), a real overlay stays unchanged while the
// scene moves for many frames. (Measured, tbench pan_slow aliased 1.5x: treating the per-pair mask as overlay cost
// 0.2 dB and raised flicker from 4.1 to 5.7 levels in a sequence without any overlay.)
float OvlStrength(float evidence) { return smoothstep(96.0 / 255.0, 160.0 / 255.0, evidence); }

float3 Hsv2Rgb(float h, float s, float v)
{
    const float3 k = saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return v * lerp(float3(1, 1, 1), k, s);
}

#endif
