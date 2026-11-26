// Original seed candidates for MoonSR. Source images are gamma-encoded and fetched with edge replication.
// Directional: compact elliptical kernel inspired by the edge-adaptive reconstruction described by AMD for FSR 1.
// Covariance: diagonal half-phase prediction inspired by Li and Orchard, IEEE TIP 2001, doi:10.1109/83.951537.
#ifndef MOONSR_ALSR_SEED_CANDIDATES
#define MOONSR_ALSR_SEED_CANDIDATES

float3 SeedFetch(int2 p)
{
    return gSeedTex.Load(int3(clamp(p, int2(0, 0), int2(gSeedSrc.xy) - 1), 0)).rgb;
}

float3 SeedDering(float3 value, float3 a, float3 b, float3 c, float3 d)
{
    const float3 lo = min(min(a, b), min(c, d));
    const float3 hi = max(max(a, b), max(c, d));
    return clamp(value, lo, hi);
}

#include "scale/lanczos_weights.hlsli"

float3 SeedLanczos(float2 f)
{
    const int2 base = int2(floor(f));
    float wx[6], wy[6];
    AxisWeights(f.x - base.x, wx);
    AxisWeights(f.y - base.y, wy);
    float3 sum = 0, lo = 1e9, hi = -1e9;
    [unroll] for (int y = 0; y < 6; ++y) {
        float3 row = 0;
        [unroll] for (int x = 0; x < 6; ++x) {
            const float3 sampleValue = SeedFetch(base + int2(x - 2, y - 2));
            row += sampleValue * wx[x];
            if (x >= 2 && x <= 3 && y >= 2 && y <= 3) {
                lo = min(lo, sampleValue);
                hi = max(hi, sampleValue);
            }
        }
        sum += row * wy[y];
    }
    return lerp(sum, clamp(sum, lo, hi), 0.8);
}

float3 SeedDirectional(float2 f)
{
    const int2 base = int2(floor(f));
    const float2 phase = f - base;
    // Central square padded with a one-pixel cross for centered gradient estimates.
    float3 samples[12];
    float luma[12];
    static const int2 offsets[12] = {
        int2(0, -1), int2(1, -1),
        int2(-1, 0), int2(0, 0), int2(1, 0), int2(2, 0),
        int2(-1, 1), int2(0, 1), int2(1, 1), int2(2, 1),
        int2(0, 2), int2(1, 2)
    };
    [unroll] for (int i = 0; i < 12; ++i) {
        samples[i] = SeedFetch(base + offsets[i]);
        luma[i] = Luma(samples[i]);
    }

    // Average unoriented edge normals using the structure tensor, so opposite gradients reinforce
    // each other on a thin line instead of cancelling.
    const float2 grad[4] = {
        float2(luma[4] - luma[2], luma[7] - luma[0]),
        float2(luma[5] - luma[3], luma[8] - luma[1]),
        float2(luma[8] - luma[6], luma[10] - luma[3]),
        float2(luma[9] - luma[7], luma[11] - luma[4])
    };
    const float4 w = float4((1 - phase.x) * (1 - phase.y), phase.x * (1 - phase.y),
                           (1 - phase.x) * phase.y, phase.x * phase.y);
    float3 tensor = 0;
    [unroll] for (int k = 0; k < 4; ++k)
        tensor += w[k] * float3(grad[k].x * grad[k].x, grad[k].y * grad[k].y, grad[k].x * grad[k].y);
    const float2 axis = float2(tensor.x - tensor.y, 2 * tensor.z);
    const float amplitude = length(axis);
    const float anisotropy = saturate(amplitude / (tensor.x + tensor.y + 0.0025));
    const float2 direction = axis / max(amplitude, 1e-6);

    float3 sum = 0;
    float sumWeight = 0;
    [unroll] for (int t = 0; t < 12; ++t) {
        const float2 delta = float2(offsets[t]) - phase;
        const float signedAxis = dot(direction, float2(delta.x * delta.x - delta.y * delta.y, 2 * delta.x * delta.y));
        const float radius2 = dot(delta, delta) * (1 + 0.25 * anisotropy) + 0.65 * anisotropy * signedAxis;
        const float window = saturate(1 - 0.25 * radius2);
        const float weight = window * window * (1 - 0.80 * radius2);
        sum += samples[t] * weight;
        sumWeight += weight;
    }
    const float3 a = samples[3], b = samples[4], c = samples[7], d = samples[8];
    const float3 fallback = lerp(lerp(a, b, phase.x), lerp(c, d, phase.x), phase.y);
    return sumWeight > 1e-3 ? SeedDering(sum / sumWeight, a, b, c, d) : fallback;
}

// Solve a positive-definite regularized covariance system using Cholesky.
float4 SeedSolveCovariance(float4x4 covariance, float4 rhs)
{
    float4x4 lower = 0;
    [unroll] for (int i = 0; i < 4; ++i) {
        [unroll] for (int j = 0; j < 4; ++j) {
            if (j <= i) {
                const float v = covariance[i][j] - dot(lower[i], lower[j]);
                lower[i][j] = i == j ? sqrt(max(v, 1e-6)) : v / max(lower[j][j], 0.001);
            }
        }
    }
    float4 y = 0, x = 0;
    [unroll] for (int a = 0; a < 4; ++a)
        y[a] = (rhs[a] - dot(lower[a], y)) / lower[a][a];
    const float4x4 upper = transpose(lower);
    [unroll] for (int b = 3; b >= 0; --b)
        x[b] = (y[b] - dot(upper[b], x)) / upper[b][b];
    return x;
}

float3 SeedCovariance(float2 f)
{
    const int2 base = int2(floor(f));
    const float2 phase = f - base;
    const float3 a = SeedFetch(base), b = SeedFetch(base + int2(1, 0));
    const float3 c = SeedFetch(base + int2(0, 1)), d = SeedFetch(base + int2(1, 1));
    const float3 bilinear = lerp(lerp(a, b, phase.x), lerp(c, d, phase.x), phase.y);

    // Fit the correction from the four diagonal neighbours to known centre samples in the
    // low-resolution patch. Value differences retain a unit DC response in flat regions.
    float luma[36];
    [unroll] for (int i = 0; i < 36; ++i)
        luma[i] = Luma(SeedFetch(base + int2(i % 6 - 2, i / 6 - 2)));
    float4x4 covariance = 0;
    float4 rhs = 0;
    [unroll] for (int y = 1; y <= 4; ++y) {
        [unroll] for (int x = 1; x <= 4; ++x) {
            const int index = y * 6 + x;
            const float4 samples = float4(luma[index - 7], luma[index - 5], luma[index + 5], luma[index + 7]);
            const float mean = dot(samples, float4(0.25, 0.25, 0.25, 0.25));
            const float4 features = samples - mean;
            rhs += features * (luma[index] - mean);
            [unroll] for (int j = 0; j < 4; ++j)
                covariance[j] += features[j] * features;
        }
    }
    [unroll] for (int k = 0; k < 4; ++k)
        covariance[k][k] += 0.04;
    const float4 coefficients = SeedSolveCovariance(covariance, rhs);
    // Apply the predicted centre correction with a tent basis, which agrees with adjacent
    // cells at the boundaries and keeps exact original samples at integer phases.
    const float2 tent = 1 - abs(2 * phase - 1);
    const float3 correction = coefficients.x * a + coefficients.y * b + coefficients.z * c + coefficients.w * d;
    return SeedDering(bilinear + tent.x * tent.y * correction, a, b, c, d);
}

#endif
