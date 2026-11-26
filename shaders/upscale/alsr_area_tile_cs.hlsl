// Tiled area back-projection. Reuse source residuals across an output thread group
// so the cubic support only pays for each source sample once per group.
#include "upscale/alsr_common.hlsli"

Texture2D<float4> gHw : register(t0);
Texture2D<float4> gWork : register(t1);
Texture2D<float4> gWorkPrev : register(t2);
Texture2D<float4> gMotion : register(t3);
Texture2D<float2> gLayer : register(t4);
RWTexture2D<float4> gHistNext : register(u0);
#ifdef ALSR_ROBUST_HISTORY
Texture2D<float4> gCurrentState : register(t5);
#endif

#include "upscale/alsr_residual.hlsli"
#ifdef ALSR_GUARDED_POINT
Texture2D<float4> gSampling : register(t5);
#include "upscale/alsr_point.hlsli"
#endif

groupshared float3 gResidualTile[20 * 12];
#ifdef ALSR_CACHE_HULL
groupshared float3 gWorkTile[20 * 12];
#endif

float3 ReconstructResidual(float2 position, int2 origin, int pitch)
{
    const float2 base = floor(position - 0.5);
    const int2 first = int2(base) - 1 - origin;
    float2 w0, w1, w2, w3;
    CubicWeights(position - 0.5 - base, -0.5, w0, w1, w2, w3);
    const float2 weights[4] = {w0, w1, w2, w3};
    float3 sum = 0, lo = 1e9, hi = -1e9;
    [unroll] for (int y = 0; y < 4; ++y) {
        float3 row = 0;
        [unroll] for (int x = 0; x < 4; ++x) {
            const float3 v = gResidualTile[(first.y + y) * pitch + first.x + x];
            row += v * weights[x].x;
            if (x >= 1 && x <= 2 && y >= 1 && y <= 2) {
                lo = min(lo, v);
                hi = max(hi, v);
            }
        }
        sum += row * weights[y].y;
    }
    return clamp(sum, lo, hi);
}

[numthreads(16, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    const int2 start = int2(group.xy) * int2(16, 8);
    const int2 outLast = int2(gOut.xy) - 1;
    const int2 end = min(start + int2(15, 7), outLast);
    const int2 first = int2(floor((float2(start) + 0.5) * gScale.zw - 0.5)) - 1;
    const int2 last = int2(floor((float2(end) + 0.5) * gScale.zw - 0.5)) + 2;
    const uint2 shape = uint2(last - first + 1);
    const int2 inLast = int2(gIn.xy) - 1;
#ifdef ALSR_PADDED_AREA
    // Map lanes straight to source rows, so no lane has to divide by the actual tile width.
    // The final row/column strips are at most 3 samples wide on each axis.
    static const int kPitch = 20;
    [unroll] for (uint oy = 0; oy <= 8; oy += 8) {
        [unroll] for (uint ox = 0; ox <= 16; ox += 16) {
            const uint2 tile = lane.xy + uint2(ox, oy);
            if (all(tile < shape)) {
                const int2 coord = clamp(first + int2(tile), 0, inLast);
                const uint address = tile.y * kPitch + tile.x;
                gResidualTile[address] = f16tof32(f32tof16(MeasurementResidual(coord)));
#ifdef ALSR_CACHE_HULL
                gWorkTile[address] = gWork.Load(int3(coord, 0)).rgb;
#endif
            }
        }
    }
#else
    const int kPitch = int(shape.x);
    [loop] for (uint i = index; i < shape.x * shape.y; i += 128) {
        const int2 coord = clamp(first + int2(i % shape.x, i / shape.x), 0, inLast);
        gResidualTile[i] = f16tof32(f32tof16(MeasurementResidual(coord)));
    }
#endif
    GroupMemoryBarrierWithGroupSync();
    const int2 pixel = start + int2(lane.xy);
    if (any(pixel > outLast)) return;
    const float4 prior = gHw.Load(int3(pixel, 0));
    const float2 pos = (float2(pixel) + 0.5) * gScale.zw;
    const int2 xi = clamp(int2(pos), 0, inLast);
#ifdef ALSR_CACHE_HULL
    const int2 workOffset = xi - first;
#endif
    float gain = 0;
    float3 residual = 0;
    const bool forceGain = (gU1.w & 1024u) != 0;
    [branch] if (prior.a > 0.0 || forceGain) {
        if (forceGain || any(gMotion.Load(int3(xi, 0)).xy != 0.0)) {
            residual = ReconstructResidual(pos, first, kPitch);
            gain = max(max(1.0 / (prior.a + 1.0), gP0.y), smoothstep(gP2.x, gP2.y, abs(LumaOf(residual))));
        } else {
#ifdef ALSR_CACHE_HULL
            const float3 current = gWorkTile[workOffset.y * kPitch + workOffset.x];
#else
            const float3 current = gWork.Load(int3(xi, 0)).rgb;
#endif
            const float difference = abs(LumaOf(current - gWorkPrev.Load(int3(xi, 0)).rgb));
            gain = smoothstep(gP2.x, gP2.y, difference);
            [branch] if (gain > 0.0) residual = ReconstructResidual(pos, first, kPitch);
        }
    }
    float3 value = prior.rgb + gain * residual;
    if ((gU1.w & 256u) == 0) {
        float3 lo = 1e9, hi = -1e9;
        [unroll] for (int k = 0; k < 9; ++k) {
            const int2 offset = int2(k % 3 - 1, k / 3 - 1);
#ifdef ALSR_CACHE_HULL
            // The residual halo already covers the full 3x3 source hull.
            const int2 samplePosition = workOffset + offset;
            const float3 c = gWorkTile[samplePosition.y * kPitch + samplePosition.x];
#else
            const int2 samplePosition = clamp(xi + offset, 0, inLast);
            const float3 c = gWork.Load(int3(samplePosition, 0)).rgb;
#endif
            lo = min(lo, c);
            hi = max(hi, c);
        }
        float3 margin = gP4.w * (hi - lo);
#ifdef ALSR_ROBUST_HISTORY
        margin += 0.20 * gCurrentState.Load(int3(pixel, 0)).x;
#endif
        value = clamp(value, lo - margin, hi + margin);
    }
    const float4 area = float4(saturate(value), min(prior.a + 1.0, gP0.w));
#ifdef ALSR_GUARDED_POINT
    const bool usePoint = gSampling.Load(int3(0, 0, 0)).x == 2.0;
    gHistNext[pixel] = BlendPoint(area, prior, pos, xi, usePoint);
#else
    gHistNext[pixel] = area;
#endif
}
