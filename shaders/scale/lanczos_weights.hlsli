#ifndef MOONSR_LANCZOS_WEIGHTS
#define MOONSR_LANCZOS_WEIGHTS

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

#endif
