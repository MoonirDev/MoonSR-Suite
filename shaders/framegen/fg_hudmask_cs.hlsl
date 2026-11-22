// Static-overlay (HUD) mask at frame resolution, computed once per real pair.
//
// Two hypotheses are compared per pixel p, with vs = mean motion of the moving cells around p (fg_motionfrac):
//   eZero(p) = max channel |I0(p) - I1(p)|                                   (the pixel did not move)
//   eMove(p) = max(max channel |I0(p) - I1(p + vs)|, |I0(p - vs) - I1(p)|)   (it moved with its surroundings;
//              the larger of the forward and backward residuals, so a moving pixel must be explained both ways
//              while an overlay pixel only needs one direction to land on something different)
//   staticPx(p) = (1 - smoothstep(lo, hi, eZero(p))) * smoothstep(m0, m1, mean3x3(eMove) - eZero(p))
// i.e. "unchanged, and clearly NOT explained by the surrounding motion". The 3x3 mean of eMove makes the test a
// patch test, so a self-similar overlay (text) is not rejected because one shifted pixel lands on another glyph.
// Inside moving texture a coincidentally unchanged pixel is explained by the motion and is rejected; flat moving
// areas are rejected because both hypotheses agree there (and warping them is harmless).
//   static(p)  = staticPx(p) * smoothstep(1, 3, number of static 8-neighbours)   (no isolated pixels)
//   edge(p)    = static(p) * smoothstep(texLo, texHi, 3x3 luma range of either frame)
//   near(p)    = max of edge over the 7x7 neighbourhood: thin HUD elements have flat interiors and are protected
//                because they lie next to static structure.
//   surround(p)= smoothstep(sLo, sHi, fraction of moving confident cells around p): an overlay is static while the
//                scene under it moves; static scenery under a still camera is not protected.
//   mask = static * near * surround
//   gSrcSize frame size   gFieldSize cell grid size
//   gP0 = (staticLo, staticHi, texLo, texHi)   gP1 = (surroundLo, surroundHi, frame px -> cell, 0)
//   margin m0..m1 = 6..16 levels
#include "framegen/fg_common.hlsli"

Texture2D<float4> gPrev : register(t0);
Texture2D<float4> gCurr : register(t1);
Texture2D<float4> gSurround : register(t2);
SamplerState gLinear : register(s0);
RWTexture2D<float> gOut : register(u0);

#define T 16
#define R 3              // dilation radius
#define B1 (T + 2 * R)   // static / edge region
#define BA (B1 + 2)      // staticPx region (+1 for the neighbour count)
#define B0 (BA + 2)      // loaded region (+1 for the 3x3 means)
groupshared float sEZ[B0 * B0];
groupshared float sEM[B0 * B0];
groupshared float sLumA[B0 * B0];
groupshared float sLumB[B0 * B0];
groupshared float sSPx[BA * BA];
groupshared float sStatic[B1 * B1];
groupshared float sEdge[B1 * B1];

float MaxAbs(float3 d) { d = abs(d); return max(d.r, max(d.g, d.b)) * 255.0; }

[numthreads(T, T, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const int2 org0 = int2(gid.xy) * T - R - 2;
    const float2 invFrame = 1.0 / float2(gSrcSize);
    for (uint i = gi; i < B0 * B0; i += T * T) {
        const int2 p = ClampI(org0 + Unflatten(int(i), B0), gSrcSize);
        const float3 a = gPrev.Load(int3(p, 0)).rgb;
        const float3 b = gCurr.Load(int3(p, 0)).rgb;
        // eMove is needed for changed pixels too: next to an overlay, the moving background's shifted sample lands on
        // the overlay, and that large residual is part of the 3x3 evidence that the overlay edge did not move.
        // (An early-out that set it to 0 for changed pixels measurably lost overlay edges.)
        const float2 pc = float2(p) + 0.5;
        const float2 vs = gSurround.SampleLevel(gLinear, pc * gP1.z / float2(gFieldSize), 0).yz;
        const float3 bf = gCurr.SampleLevel(gLinear, (pc + vs) * invFrame, 0).rgb;
        const float3 ab = gPrev.SampleLevel(gLinear, (pc - vs) * invFrame, 0).rgb;
        sEZ[i] = MaxAbs(a - b);
        sEM[i] = max(MaxAbs(a - bf), MaxAbs(ab - b));
        sLumA[i] = Luma(a) * 255.0;
        sLumB[i] = Luma(b) * 255.0;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint j0 = gi; j0 < BA * BA; j0 += T * T) {
        const int2 q = Unflatten(int(j0), BA) + 1;
        float em = 0.0;
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) em += sEM[(q.y + y) * B0 + q.x + x];
        }
        const float ez = sEZ[q.y * B0 + q.x];
        sSPx[j0] = (1.0 - smoothstep(gP0.x, gP0.y, ez)) * smoothstep(6.0, 16.0, em / 9.0 - ez);
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint j = gi; j < B1 * B1; j += T * T) {
        const int2 q1 = Unflatten(int(j), B1);
        const int2 qa = q1 + 1; // in sSPx
        const int2 q0 = q1 + 2; // in the loaded region
        float loA = 1e9, hiA = -1e9, loB = 1e9, hiB = -1e9, nStatic = 0.0;
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                const uint k = uint((q0.y + y) * B0 + q0.x + x);
                loA = min(loA, sLumA[k]);
                hiA = max(hiA, sLumA[k]);
                loB = min(loB, sLumB[k]);
                hiB = max(hiB, sLumB[k]);
                if (x != 0 || y != 0) nStatic += sSPx[(qa.y + y) * BA + qa.x + x];
            }
        }
        const float st = sSPx[qa.y * BA + qa.x] * smoothstep(1.0, 3.0, nStatic);
        sStatic[j] = st;
        sEdge[j] = st * smoothstep(gP0.z, gP0.w, max(hiA - loA, hiB - loB));
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 p = int2(gid.xy) * T + int2(gtid.xy);
    if (any(p >= gSrcSize)) return;
    const float st = sStatic[(gtid.y + R) * B1 + gtid.x + R];
    // A zero static weight makes the final mask zero regardless of the dilation or surround.
    // Most scene pixels are not static HUD, so avoid the 7x7 search for them.
    if (st == 0.0) {
        gOut[p] = 0.0;
        return;
    }
    float nearEdge = 0.0;
    [loop] for (int y = 0; y <= 2 * R; ++y) {
        [unroll] for (int x = 0; x <= 2 * R; ++x) nearEdge = max(nearEdge, sEdge[(gtid.y + y) * B1 + gtid.x + x]);
    }
    const float2 cellUv = (float2(p) + 0.5) * gP1.z / float2(gFieldSize);
    const float surround = smoothstep(gP1.x, gP1.y, gSurround.SampleLevel(gLinear, cellUv, 0).x);
    gOut[p] = st * nearEdge * surround;
}
