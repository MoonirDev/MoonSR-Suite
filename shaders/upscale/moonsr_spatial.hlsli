// MoonSR Spatial v0.1 core (steps 2-5 of moonsr_spatial_ps.hlsl, which documents the algorithm). Owner:
// upscaling-engineer. Shared by moonsr_spatial_ps.hlsl (ScalePass "MoonSR Spatial") and the Moon ALSR seed
// (alsr_seed.hlsli), so the algorithm exists once. Needs Luma() (gamma luma, 0.299 / 0.587 / 0.114) from the includer.
//   C        : the 4x4 texels around the sample, C[j * 4 + i] = texel (floor(f) - 1 + (i, j)), f = sample position
//              in texel units with centres at integers
//   t        : f - floor(f)
//   mag      : min magnification of the two axes (dst / src)
//   strength : adaptivity (default 1), sigma: luma noise sigma of the coherence regulariser (default 0.015)
#ifndef MOONSR_SPATIAL_HLSLI
#define MOONSR_SPATIAL_HLSLI

static const float kNoiseSigmaDefault = 0.015;
static const float kStrengthDefault = 1.0;
static const float kCohLo = 0.35;
static const float kCohHi = 0.85;
static const float kEdgeLo = 0.02;
static const float kEdgeHi = 0.12;
static const float kStretchMax = 0.5;
static const float kMagRamp = 4.0;
static const float kDeringSoft = 0.1;

// Isotropic 1D profile: (1 - x^2)(1 - x^2/4)^4. Zero at |x| = 1 and 2 (interpolating).
float KIso(float x)
{
    float x2 = x * x;
    float w = saturate(1.0 - 0.25 * x2);
    w *= w;
    return (1.0 - x2) * w * w;
}

// Support guard: 1 - (x^2/4)^3, clamped to 0.
float Guard(float x)
{
    float a = 0.25 * x * x;
    return saturate(1.0 - a * a * a);
}

float3 SoftClamp(float3 c, float3 lo, float3 hi)
{
    float3 ov = kDeringSoft * (hi - lo) + 1e-6;
    float3 eh = max(c - hi, 0.0);
    float3 el = max(lo - c, 0.0);
    return clamp(c, lo, hi) + eh * ov / (ov + eh) - el * ov / (ov + el);
}

float3 MoonsrSpatial(float3 C[16], float2 t, float mag, float strength, float sigma)
{
    float L[16];
    [unroll]
    for (int n = 0; n < 16; ++n)
        L[n] = Luma(C[n]);

    // ---- 2. structure tensor ----
    const float3 wx = float3(0.5 * (1.0 - t.x) * (1.0 - t.x), 0.75 - (t.x - 0.5) * (t.x - 0.5), 0.5 * t.x * t.x);
    const float3 wy = float3(0.5 * (1.0 - t.y) * (1.0 - t.y), 0.75 - (t.y - 0.5) * (t.y - 0.5), 0.5 * t.y * t.y);
    float jxx = 0.0, jxy = 0.0, jyy = 0.0;
    [unroll]
    for (int cy = 0; cy < 3; ++cy)
    {
        [unroll]
        for (int cx = 0; cx < 3; ++cx)
        {
            const float a = L[cy * 4 + cx], b = L[cy * 4 + cx + 1];
            const float c = L[(cy + 1) * 4 + cx], d = L[(cy + 1) * 4 + cx + 1];
            const float gx = 0.5 * ((b - a) + (d - c));
            const float gy = 0.5 * ((c - a) + (d - b));
            const float w = wx[cx] * wy[cy];
            jxx += w * gx * gx;
            jxy += w * gx * gy;
            jyy += w * gy * gy;
        }
    }

    const float trace = jxx + jyy;
    const float dxx = jxx - jyy;
    const float disc = sqrt(dxx * dxx + 4.0 * jxy * jxy);
    const float coh = disc / (trace + 2.0 * sigma * sigma + 1e-12);
    const float lam1 = 0.5 * (trace + disc);

    // ---- 3. confidence ----
    const float A = saturate(smoothstep(kCohLo, kCohHi, coh) * saturate((mag - 1.0) * kMagRamp) * strength);
    const float E = smoothstep(kEdgeLo, kEdgeHi, sqrt(lam1));
    const float gamma = 1.0 - E;

    // ---- 4a. isotropic fallback (always) ----
    float hx[4], hy[4], gdx[4], gdy[4];
    [unroll]
    for (int m = 0; m < 4; ++m)
    {
        const float ox = float(m) - 1.0 - t.x;
        const float oy = float(m) - 1.0 - t.y;
        hx[m] = KIso(ox);
        hy[m] = KIso(oy);
        gdx[m] = Guard(ox);
        gdy[m] = Guard(oy);
    }
    float3 cIso = 0.0;
    float wIso = 0.0;
    [unroll]
    for (int j = 0; j < 4; ++j)
    {
        [unroll]
        for (int k = 0; k < 4; ++k)
        {
            const float w = hx[k] * hy[j];
            cIso += w * C[j * 4 + k];
            wIso += w;
        }
    }
    float3 color = cIso / wIso;

    // ---- 4b. anisotropic (only where it contributes) ----
    [branch]
    if (A > 0.0)
    {
        const float rd = 1.0 / max(disc, 1e-10);
        const float c2 = dxx * rd;
        const float s2 = 2.0 * jxy * rd;
        const float nx = sqrt(saturate(0.5 + 0.5 * c2));
        const float ny = (s2 < 0.0 ? -1.0 : 1.0) * sqrt(saturate(0.5 - 0.5 * c2));
        const float kap = 1.0 + A * (abs(nx) + abs(ny) - 1.0);
        const float invS = 1.0 / (1.0 + kStretchMax * A);
        const float2 ua = float2(nx, ny) * kap;   // across axis, pre-scaled
        const float2 va = float2(-ny, nx) * invS; // along axis, pre-scaled

        float3 cAni = 0.0;
        float wAni = 0.0;
        [unroll]
        for (int j = 0; j < 4; ++j)
        {
            [unroll]
            for (int k = 0; k < 4; ++k)
            {
                const float2 d = float2(float(k) - 1.0 - t.x, float(j) - 1.0 - t.y);
                const float u = dot(d, ua);
                const float v = dot(d, va);
                const float p2 = u * u + v * v;
                const float wp = saturate(1.0 - 0.25 * p2);
                const float w = (1.0 - p2) * wp * wp * wp * (1.0 - gamma * 0.25 * p2) * gdx[k] * gdy[j];
                cAni += w * C[j * 4 + k];
                wAni += w;
            }
        }
        color = lerp(color, cAni / max(wAni, 1e-4), A);
    }

    // ---- 5. deringing: continuous nearest-2x2 bounds ----
    const float3 bil = lerp(lerp(C[5], C[6], t.x), lerp(C[9], C[10], t.x), t.y);
    float3 lo = bil, hi = bil;
    float tx4[4], ty4[4];
    [unroll]
    for (int q = 0; q < 4; ++q)
    {
        tx4[q] = saturate(1.5 - abs(float(q) - 1.0 - t.x));
        ty4[q] = saturate(1.5 - abs(float(q) - 1.0 - t.y));
    }
    [unroll]
    for (int j2 = 0; j2 < 4; ++j2)
    {
        [unroll]
        for (int k2 = 0; k2 < 4; ++k2)
        {
            const float3 mb = lerp(bil, C[j2 * 4 + k2], tx4[k2] * ty4[j2]);
            lo = min(lo, mb);
            hi = max(hi, mb);
        }
    }
    return SoftClamp(color, lo, hi);
}

#endif
