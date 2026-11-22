// Lanczos-3 scaling, single pass, 6x6 taps via LoadSrc (36 point loads), anti-ringing clamp.
//
// Kernel: L(x) = sinc(x) * sinc(x/3) for |x| < 3, else 0, with sinc(x) = sin(pi x)/(pi x).
// Combined form used here: L(x) = 3 sin(pi x) sin(pi x / 3) / (pi^2 x^2), and L(0) = 1 exactly
// (handled with an explicit select; no division by zero is ever evaluated into the result).
// Weights are computed per axis (separable), each axis normalised by the sum of its 6 weights, so the
// 2D weights sum to 1 and brightness is preserved even at region borders (LoadSrc replicates edges).
//
// Anti-ringing: result is clamped to [min,max] of the central 2x2 texels, blended with AR_STRENGTH.
//
// Limitation: the kernel is NOT widened when downscaling (srcRectSize > dstSize on an axis), so
// downscaling will alias. Intended use is upscaling.
// Cost: 36 point loads, no filtered fetches.
#include "common/scale_common.hlsli"

static const float AR_STRENGTH = 0.8;
static const float PI = 3.14159265358979;

float Lanczos3(float x)
{
    float ax = abs(x);
    float px = PI * ax;
    float safeX = max(ax, 1e-6);                       // keeps the unused branch finite
    float v = 3.0 * sin(px) * sin(px * (1.0 / 3.0)) / (PI * PI * safeX * safeX);
    v = (ax == 0.0) ? 1.0 : v;                         // exact value at the singularity
    return (ax < 3.0) ? v : 0.0;
}

void AxisWeights(float f, out float w[6])
{
    // Taps sit at base-2 .. base+3; distance from the sample point to tap k is (k - f).
    float sum = 0.0;
    [unroll] for (int k = 0; k < 6; ++k)
    {
        w[k] = Lanczos3(float(k - 2) - f);
        sum += w[k];
    }
    float inv = 1.0 / sum;
    [unroll] for (int j = 0; j < 6; ++j)
        w[j] *= inv;
}

float4 main(VsOut i) : SV_Target
{
    float2 pos = SrcPosTexels(i.uv) - 0.5;   // in texel-index space (texel centres at integers)
    float2 fl = floor(pos);
    float2 f = pos - fl;
    int2 base = int2(fl);

    float wx[6], wy[6];
    AxisWeights(f.x, wx);
    AxisWeights(f.y, wy);

    float3 acc = 0.0;
    float3 mn = 1e9, mx = -1e9;
    [unroll] for (int y = 0; y < 6; ++y)
    {
        float3 row = 0.0;
        [unroll] for (int x = 0; x < 6; ++x)
        {
            float3 t = LoadSrc(base + int2(x - 2, y - 2));
            row += t * wx[x];
            if (x >= 2 && x <= 3 && y >= 2 && y <= 3)
            {
                mn = min(mn, t);
                mx = max(mx, t);
            }
        }
        acc += row * wy[y];
    }

    acc = lerp(acc, clamp(acc, mn, mx), AR_STRENGTH);
    return float4(saturate(acc), 1.0);
}
