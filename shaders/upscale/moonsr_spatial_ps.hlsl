// MoonSR Spatial v0.1 - edge-directed adaptive reconstruction, single pass. FIRST ITERATION (see "Status" below).
// Owner: upscaling-engineer. Reference port (must stay numerically in sync): benchmarks/reference/moonsr_spatial_ref.py
//
// Input: gamma-encoded SDR source region (no motion vectors, depth or jitter). Output: one destination pixel.
// Intended range: 1.3x - 3x magnification of anti-aliased game images. At exactly 1:1 it is an exact pass-through.
//
// Algorithm
//   1. Taps: the 4x4 texels around the sample position (16 RGB texels; luma derived from them).
//      Fast path: 12 Gathers (R, G, B x 4 quads). Within 2 texels of the source-region border, 16 clamped Loads
//      instead (Gather has no region clamp; the slow path is clamp-to-edge exactly like LoadSrc). Both paths are exact.
//   2. Structure tensor: 2x2 box derivatives (gx, gy) at the 3x3 texel-cell corners inside the 4x4 block, accumulated
//      as J = sum w * [gx^2 gxgy; gxgy gy^2] with separable quadratic B-spline weights in the sub-texel position t.
//      These weights are continuous (C1) across texel cells, so the tensor - and hence the kernel - never jumps at
//      cell boundaries (no grid seams), and they never collapse onto a single gradient sample (a rank-1 tensor would
//      fake perfect coherence).
//        coherence c = sqrt((Jxx-Jyy)^2 + 4Jxy^2) / (Jxx + Jyy + 2*sigma_n^2)   (noise-regularised)
//        lambda1     = ((Jxx+Jyy) + sqrt(...)) / 2                               (dominant gradient energy)
//        across-edge unit vector n from the double-angle form (no atan2).
//   3. Confidence:
//        A = smoothstep(0.35, 0.85, c) * saturate(4*(mag - 1)) * strength          (anisotropy, 0..1)
//        E = smoothstep(0.02, 0.12, sqrt(lambda1))                                   (edge strength)
//      mag = min magnification of the two axes: anisotropy fades in between 1.0x and 1.25x, so 1:1 is pass-through.
//   4. Kernels (offset d = texel centre - sample position, in texels):
//      Isotropic fallback (separable, interpolating, Lanczos-2-like):
//        h(x) = (1 - x^2) (1 - x^2/4)^4 ;  K_iso = h(dx) h(dy)
//      Anisotropic (elliptical, radial profile in an edge-aligned metric):
//        u = d.n * kappa, v = d.t / S          t = edge tangent
//        kappa = 1 + A (|nx| + |ny| - 1)       across-edge narrowing = density of the texel lattice projected on n
//                                              (1 on axis-aligned edges, sqrt(2) on 45-degree edges)
//        S     = 1 + 0.5 A                     along-edge stretch (smooths stair steps, adds no along-edge lobe)
//        rho^2 = u^2 + v^2
//        g(rho) = (1 - rho^2) (1 - rho^2/4)^3 (1 - (1-E) rho^2/4)   E=0: same lobe as h, E=1: stronger negative lobe
//        K_ani = g(rho) * G(dx) G(dy),  G(x) = 1 - (x^2/4)^3   (flat-topped support guard, exactly 0 at |x| = 2,
//                                                               keeps weights continuous when the 4x4 window moves)
//      Result = lerp(normalised K_iso sum, normalised K_ani sum, A). Low coherence (corners, junctions, single dots,
//      text, noise) therefore falls back smoothly to the isotropic kernel, independent of the (meaningless) direction.
//   5. Deringing: per-channel soft clamp to a continuous version of the nearest-2x2 [min, max]. A hard 2x2 window
//      switches when the sample crosses a texel centre and caused output jumps up to 0.10 (seams) in the reference.
//      Membership f = tent(dx) tent(dy), tent(x) = saturate(1.5 - |x|); each of the 16 texels contributes
//      lerp(bil, C, f) to min and max (bil = bilinear value). Exactly the 2x2 min/max at cell centres; at texel centres
//      the bounds reach half-way to the neighbours. Overshoot e beyond the bounds is compressed to e*o/(o+e) with
//      o = 0.1 * (max - min), i.e. at most 10% of the local range survives.
//
// Parameters (constant buffer; 0 = default): param1 = adaptivity strength (default 1.0, scales A, clamped to 0..1
// after scaling), param2 = luma noise sigma for the coherence regulariser (default 0.015). `sharpness` is NOT used
// here: the generic sharpen pass owns it. param3 unused.
//
// Status: first iteration. Not tuned on real game captures yet; not temporally stabilised; no chroma-specific handling
// (luma drives the geometry, the kernel is applied to RGB); downscaling (mag < 1) uses the isotropic kernel without a
// prefilter and will alias.
//
// Originality / references: no code or constants from FSR (EASU/RCAS), NIS, SGSR or any other SDK. Ideas from the
// public literature: structure tensor and coherence (Bigun & Granlund 1987; Knutsson 1989; Weickert, "Anisotropic
// Diffusion in Image Processing", 1998), elliptical / anisotropic kernel regression (Takeda, Farsiu, Milanfar,
// "Kernel Regression for Image Processing and Reconstruction", IEEE TIP 2007), elliptical weighted averaging (Greene &
// Heckbert 1986), min/max anti-ringing clamp (common practice). The kernel polynomials are original to this file.
#include "common/scale_common.hlsli"
#include "upscale/moonsr_spatial.hlsli"

float4 main(VsOut i) : SV_Target
{
    const float2 f = SrcPosTexels(i.uv) - 0.5;
    const float2 base = floor(f);
    const float2 t = f - base;

    // ---- 1. 4x4 neighbourhood: C[j*4+i] = texel (base - 1 + (i, j)) ----
    float3 C[16];
    const float2 rectLo = srcRectOrigin;
    const float2 rectHi = srcRectOrigin + srcRectSize - 1.0;
    [branch]
    if (all(base - 1.0 >= rectLo) && all(base + 2.0 <= rectHi))
    {
        // Gather at the texel corner `base + 2*(qx,qy)` returns texels base-1+2q .. base+2q (both axes).
        [unroll]
        for (int qy = 0; qy < 2; ++qy)
        {
            [unroll]
            for (int qx = 0; qx < 2; ++qx)
            {
                const float2 uv = (base + float2(2 * qx, 2 * qy)) * srcTexelSize;
                const float4 r = srcTex.GatherRed(pointClamp, uv);
                const float4 g = srcTex.GatherGreen(pointClamp, uv);
                const float4 b = srcTex.GatherBlue(pointClamp, uv);
                const int k = (2 * qy) * 4 + 2 * qx;
                C[k]     = float3(r.w, g.w, b.w); // top-left
                C[k + 1] = float3(r.z, g.z, b.z); // top-right
                C[k + 4] = float3(r.x, g.x, b.x); // bottom-left
                C[k + 5] = float3(r.y, g.y, b.y); // bottom-right
            }
        }
    }
    else
    {
        const int2 ib = int2(base) - 1;
        [unroll]
        for (int j = 0; j < 4; ++j)
        {
            [unroll]
            for (int k = 0; k < 4; ++k)
                C[j * 4 + k] = LoadSrc(ib + int2(k, j));
        }
    }

    const float2 magXY = dstSize / srcRectSize;
    return float4(MoonsrSpatial(C, t, min(magXY.x, magXY.y), param1 > 0.0 ? param1 : kStrengthDefault,
                                param2 > 0.0 ? param2 : kNoiseSigmaDefault), 1.0);
}
