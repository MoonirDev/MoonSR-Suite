// Catmull-Rom bicubic scaling with a light anti-ringing clamp.
//
// Technique: the separable 4x4 Catmull-Rom kernel (16 taps) is evaluated with 9 bilinear fetches by
// merging the two centre taps of each axis into one fetch placed at the weighted centroid
// (w1,w2 -> weight w1+w2 at offset w2/(w1+w2)). This is a published, general technique (it follows from
// the linearity of bilinear filtering). Corners are NOT dropped, so the result equals the full kernel.
// Cost: 9 bilinear fetches + 4 point loads (anti-ringing neighbourhood) = 13 fetches.
//
// Border handling: the taps are placed at texel-space positions and SampleSrcLinear() clamps each
// position to [origin+0.5, origin+size-0.5]. Clamping a *position* to the centre of the edge texel makes
// the bilinear fetch return exactly that edge texel. For an unclamped merged tap spanning texels (a, a+1)
// where a+1 lies outside the region, edge replication would give mix(T[a], T[a+1]=T[a]) = T[a]; the clamp
// moves the tap onto the centre of texel a and returns T[a]. Likewise a merged tap with a below the region
// is moved onto texel a+1. Outer taps (tc-1, tc+2) are moved onto the nearest edge texel. So every tap
// reproduces clamp-to-edge (replicate) addressing exactly and no energy is lost or redistributed: the
// weights still sum to 1. (The sampler is never allowed to blend with texels outside the source region.)
//
// Anti-ringing: the result is clamped to [min,max] of the 4 nearest source texels, blended in with
// AR_STRENGTH (1 = full clamp, 0 = none). A partial strength keeps a little of the Catmull-Rom sharpness.
#include "common/scale_common.hlsli"

static const float AR_STRENGTH = 0.75;

float3 BicubicTap(float2 p) { return SampleSrcLinear(p); }

float4 main(VsOut i) : SV_Target
{
    float2 pos = SrcPosTexels(i.uv);
    float2 tc = floor(pos - 0.5) + 0.5;   // centre of the texel at or below pos
    float2 f = pos - tc;                  // 0..1 between texel centres tc and tc+1
    float2 f2 = f * f;
    float2 f3 = f2 * f;

    // Catmull-Rom weights (tension 0.5).
    float2 w0 = -0.5 * f3 + f2 - 0.5 * f;
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    float2 w3 = 0.5 * f3 - 0.5 * f2;

    float2 w12 = w1 + w2;
    float2 p0 = tc - 1.0;
    float2 p12 = tc + w2 / w12;
    float2 p3 = tc + 2.0;

    float3 c = 0.0;
    c += BicubicTap(float2(p0.x,  p0.y))  * (w0.x  * w0.y);
    c += BicubicTap(float2(p12.x, p0.y))  * (w12.x * w0.y);
    c += BicubicTap(float2(p3.x,  p0.y))  * (w3.x  * w0.y);
    c += BicubicTap(float2(p0.x,  p12.y)) * (w0.x  * w12.y);
    c += BicubicTap(float2(p12.x, p12.y)) * (w12.x * w12.y);
    c += BicubicTap(float2(p3.x,  p12.y)) * (w3.x  * w12.y);
    c += BicubicTap(float2(p0.x,  p3.y))  * (w0.x  * w3.y);
    c += BicubicTap(float2(p12.x, p3.y))  * (w12.x * w3.y);
    c += BicubicTap(float2(p3.x,  p3.y))  * (w3.x  * w3.y);

    // Anti-ringing against the 4 nearest texels.
    int2 b = int2(floor(tc));
    float3 t00 = LoadSrc(b);
    float3 t10 = LoadSrc(b + int2(1, 0));
    float3 t01 = LoadSrc(b + int2(0, 1));
    float3 t11 = LoadSrc(b + int2(1, 1));
    float3 mn = min(min(t00, t10), min(t01, t11));
    float3 mx = max(max(t00, t10), max(t01, t11));
    c = lerp(c, clamp(c, mn, mx), AR_STRENGTH);

    return float4(saturate(c), 1.0);
}
