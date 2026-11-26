// Moon ALSR pass 6 (output resolution): adaptive sharpening, encoding and debug views. Owner: upscaling-engineer.
// Output: the caller's destination, including for RenderSingle.
// Input t0: always gamma-encoded: the history's gamma copy (alsr_update MRT1, gU1.z == 0; alpha = count / maxCount) or
// a single-frame reconstruction (gU1.z == 1).
//
// Sharpening (on gamma-encoded values, i.e. perceptually; original design, same family as scale/sharpen_ps.hlsl):
//   avg  = mean of the 4 direct neighbours, C = luma range of the 3x3 neighbourhood
//   k    = sharpness * 1.25 * (1 - smoothstep(0.35, 0.9, C)) * smoothstep(0.01, 0.06, C)
//   out  = c + k (c - avg), clamped per channel to the 3x3 min / max  -> no over- or undershoot (no ringing halo)
// sharpness 0 gives out == c exactly.
// Debug views (gU0.w): 1 = accumulated count (black 0 .. green max), 2 = rejection (red = history rejected, over
// grey luma), 3 = motion (hue = direction, saturation = magnitude up to 8 source px, value = confidence; with the
// development flag gU1.w & 1 instead the raw encoding r = 0.5 + vx / 4, g = 0.5 + vy / 4 (source px), b = confidence).
#include "upscale/alsr_common.hlsli"

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> gIn0 : register(t0);
Texture2D<float> gTrust : register(t1);
Texture2D<float4> gMotion : register(t2);

float3 Fetch(int2 p)
{
    return gIn0.Load(int3(clamp(p, int2(0, 0), int2(gOut.xy) - 1), 0)).rgb;
}

float4 main(VsOut i) : SV_Target
{
    const int2 p = int2(i.pos.xy);
    float3 t[9];
    float lmin = 1e9, lmax = -1e9;
    float3 cmin = 1e9, cmax = -1e9;
    [unroll] for (int k = 0; k < 9; ++k) {
        t[k] = Fetch(p + int2(k % 3 - 1, k / 3 - 1));
        const float l = LumaOf(t[k]);
        lmin = min(lmin, l);
        lmax = max(lmax, l);
        cmin = min(cmin, t[k]);
        cmax = max(cmax, t[k]);
    }
    const float3 c = t[4];
    const float3 avg = 0.25 * (t[1] + t[3] + t[5] + t[7]);
    const float contrast = lmax - lmin;
    const float kk = saturate(gP0.x) * 1.25 * (1.0 - smoothstep(0.35, 0.9, contrast)) * smoothstep(0.01, 0.06, contrast);
    float3 o = clamp(c + kk * (c - avg), cmin, cmax);

    const uint view = gU0.w;
    if (view == 1 && gU1.z == 0) {
        const float a = gIn0.Load(int3(p, 0)).a;
        o = float3(0.15 * LumaOf(c), a, 0.15 * LumaOf(c));
    } else if (view == 2 && gU1.z == 0) {
        const float tr = gTrust.Load(int3(p, 0));
        const float l = 0.5 * LumaOf(c);
        o = lerp(float3(1.0, 0.0, 0.0), float3(l, l, l), tr);
    } else if (view == 3 && gU1.z == 0) {
        const int2 xi = clamp(int2(i.pos.xy * gScale.zw), int2(0, 0), int2(gIn.xy) - 1);
        const float4 m = gMotion.Load(int3(xi, 0));
        if ((gU1.w & 1u) != 0) {
            // Development: raw encoding for measurement (tbench --motion-diag): 0.5 + v / 4 (source px), confidence.
            o = float3(saturate(0.5 + m.x * 0.25), saturate(0.5 + m.y * 0.25), saturate(m.z));
        } else {
            const float ang = atan2(m.y, m.x) / 6.2831853 + 0.5;
            o = Hsv2Rgb(ang, saturate(length(m.xy) / 8.0), 0.2 + 0.8 * saturate(m.z));
        }
    }
    return float4(o, 1.0);
}
