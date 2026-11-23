// Fit the dominant plane homography to the frame generator's backward flow, then
// intersect the residual lines to estimate the epipole. No CPU readback or new flow.
#include "relief/relief_common.hlsli"
#include "relief/relief_motion.hlsli"

Texture2DArray<float4> flowTex : register(t0);
Texture2D<float4> gateTex       : register(t1);
RWByteAddressBuffer outFit      : register(u0);
RWByteAddressBuffer frameState   : register(u1);
SamplerState linClamp           : register(s0);

#define LANES 64
#define TERMS 45 // upper triangle of the 8x8 normal matrix, right-hand side, and total weight
groupshared float partial[LANES * TERMS];
groupshared float3 fitX, fitY;
groupshared float2 fitW;
groupshared uint fitValid;

uint2 SampleCell(uint i, uint2 grid)
{
    return min(uint2((float2(i % grid.x, i / grid.x) + 0.5) * float2(cCells) / float2(grid)), cCells - 1);
}

float SampleWeight(float4 flow, float2 p)
{
    const float hud = gateTex.SampleLevel(linClamp, p / float2(cSize), 0).x;
    return smoothstep(0.25, 0.75, flow.z) * saturate(1.0 - flow.w) * hud;
}

// Only the group leader solves the small, pivoted system. Degenerate flow coverage
// is rejected instead of permitting a singular fit to amplify residuals.
void Solve()
{
    float systemMatrix[72]; // augmented 8 x 9, private to the leader
    [unroll] for (uint i = 0; i < 72; ++i) systemMatrix[i] = 0.0;
    uint k = 0;
    [loop] for (uint r = 0; r < 8; ++r) {
        [loop] for (uint c = r; c < 8; ++c) {
            const float v = partial[k++];
            systemMatrix[r * 9 + c] = v;
            systemMatrix[c * 9 + r] = v;
        }
        systemMatrix[r * 9 + 8] = partial[36 + r];
    }
    fitValid = partial[44] >= 12.0 ? 1u : 0u;
    [loop] for (uint p = 0; p < 8 && fitValid != 0; ++p) {
        uint pivot = p;
        [loop] for (uint candidate = p + 1; candidate < 8; ++candidate)
            if (abs(systemMatrix[candidate * 9 + p]) > abs(systemMatrix[pivot * 9 + p])) pivot = candidate;
        const float diagonal = systemMatrix[pivot * 9 + p];
        if (abs(diagonal) < 1e-4) {
            fitValid = 0;
            break;
        }
        [loop] for (uint c = p; c < 9; ++c) {
            const float v = systemMatrix[pivot * 9 + c] / diagonal;
            systemMatrix[pivot * 9 + c] = systemMatrix[p * 9 + c];
            systemMatrix[p * 9 + c] = v;
        }
        [loop] for (uint r = 0; r < 8; ++r) {
            if (r == p) continue;
            const float v = systemMatrix[r * 9 + p];
            [loop] for (uint c = p + 1; c < 9; ++c)
                systemMatrix[r * 9 + c] -= v * systemMatrix[p * 9 + c];
        }
    }
    if (fitValid != 0) {
        fitX = float3(systemMatrix[8], systemMatrix[17], systemMatrix[26]);
        fitY = float3(systemMatrix[35], systemMatrix[44], systemMatrix[53]);
        fitW = float2(systemMatrix[62], systemMatrix[71]);
    }
}

[numthreads(LANES, 1, 1)]
void main(uint lane : SV_GroupIndex)
{
    const uint2 grid = min(cCells, uint2(32, 24));
    const uint count = grid.x * grid.y;
    if (lane == 0) {
        fitX = 0.0;
        fitY = 0.0;
        fitW = 0.0;
        fitValid = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Three passes of iteratively reweighted least squares (Huber), sampling the
    // entire screen on a bounded grid so the cost does not grow with render size.
    [loop] for (uint iteration = 0; iteration < 3; ++iteration) {
        float accum[TERMS];
        [unroll] for (uint a = 0; a < TERMS; ++a) accum[a] = 0.0;
        for (uint base = 0; base < count; base += LANES) {
            const uint i = base + lane;
            const uint2 cell = SampleCell(i, grid);
            const float2 position = MotionCellPosition(cell);
            const float4 flow = flowTex.Load(int4(cell, 1, 0));
            float weight = i < count ? SampleWeight(flow, position) : 0.0;
            const float2 p = MotionNormalPosition(position);
            if (iteration > 0 && fitValid != 0) {
                const float error = length(flow.xy - PlaneFlow(p, fitX, fitY, fitW));
                weight *= min(1.0, 0.8 / max(error, 1e-4));
            }
            const float2 d = 2.0 * flow.xy / float2(cSize);
            const float2 dst = p + d;
            float rowX[8] = {p.x, p.y, 1.0, 0.0, 0.0, 0.0, -dst.x * p.x, -dst.x * p.y};
            float rowY[8] = {0.0, 0.0, 0.0, p.x, p.y, 1.0, -dst.y * p.x, -dst.y * p.y};
            uint k = 0;
            [unroll] for (uint r = 0; r < 8; ++r) {
                [unroll] for (uint c = r; c < 8; ++c)
                    accum[k++] += weight * (rowX[r] * rowX[c] + rowY[r] * rowY[c]);
                accum[36 + r] += weight * (rowX[r] * d.x + rowY[r] * d.y);
            }
            accum[44] += weight;
        }
        [unroll] for (uint term = 0; term < TERMS; ++term) partial[lane * TERMS + term] = accum[term];
        GroupMemoryBarrierWithGroupSync();
        if (lane < TERMS) {
            float total = partial[lane];
            [loop] for (uint i = 1; i < LANES; ++i) total += partial[i * TERMS + lane];
            partial[lane] = total;
        }
        GroupMemoryBarrierWithGroupSync();
        if (lane == 0) Solve();
        GroupMemoryBarrierWithGroupSync();
    }

    // Least-squares intersection of the lines perpendicular to the residual
    // normals. Parallel lines are the limit case of a lateral camera move.
    float3 lineNormal = 0.0;
    float2 rhs = 0.0;
    float weightSum = 0.0, residualSum = 0.0;
    for (uint base = 0; base < count; base += LANES) {
        const uint i = base + lane;
        const uint2 cell = SampleCell(i, grid);
        const float2 p = MotionCellPosition(cell);
        const float4 flow = flowTex.Load(int4(cell, 1, 0));
        const float2 res = flow.xy - PlaneFlow(MotionNormalPosition(p), fitX, fitY, fitW);
        const float len = length(res);
        const float weight = i < count ? SampleWeight(flow, p) : 0.0;
        const float lineWeight = weight * smoothstep(0.15, 0.6, len) * min(1.0, 4.0 / max(len, 1e-4));
        const float2 n = float2(-res.y, res.x) / max(len, 1e-4);
        lineNormal += lineWeight * float3(n.x * n.x, n.x * n.y, n.y * n.y);
        rhs += lineWeight * n * dot(n, p);
        weightSum += weight;
        residualSum += weight * min(len, 4.0);
    }
    partial[lane * TERMS + 0] = lineNormal.x;
    partial[lane * TERMS + 1] = lineNormal.y;
    partial[lane * TERMS + 2] = lineNormal.z;
    partial[lane * TERMS + 3] = rhs.x;
    partial[lane * TERMS + 4] = rhs.y;
    partial[lane * TERMS + 5] = weightSum;
    partial[lane * TERMS + 6] = residualSum;
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0) {
        [loop] for (uint i = 1; i < LANES; ++i) {
            lineNormal += float3(partial[i * TERMS], partial[i * TERMS + 1], partial[i * TERMS + 2]);
            rhs += float2(partial[i * TERMS + 3], partial[i * TERMS + 4]);
            weightSum += partial[i * TERMS + 5];
            residualSum += partial[i * TERMS + 6];
        }
        const float tau = residualSum / max(weightSum, 1e-4);
        uint mode = 3;
        float2 axis = 0.0;
        const bool cut = asfloat(frameState.Load(RST_CUT)) > 0.5;
        if (!cut && fitValid != 0 && weightSum > 0.15 * float(count)) {
            mode = 0;
            if (tau > 0.3) {
                const float trace = lineNormal.x + lineNormal.z;
                const float det = lineNormal.x * lineNormal.z - lineNormal.y * lineNormal.y;
                if (det > 0.015 * trace * trace) {
                    axis = float2(lineNormal.z * rhs.x - lineNormal.y * rhs.y,
                                  lineNormal.x * rhs.y - lineNormal.y * rhs.x) / det;
                    mode = 1;
                } else if (trace > 1e-4) {
                    const float2 normal = lineNormal.x >= lineNormal.z ? lineNormal.xy : lineNormal.yz;
                    axis = normalize(float2(normal.y, -normal.x));
                    if (dot(axis, float2(fitX.z, fitY.z) * cSize) < 0.0) axis = -axis;
                    mode = 2;
                }
            }
        }
        outFit.Store3(FIT_X, asuint(fitX));
        outFit.Store3(FIT_Y, asuint(fitY));
        outFit.Store2(FIT_W, asuint(fitW));
        outFit.Store2(FIT_AXIS, asuint(axis));
        outFit.Store(FIT_MODE, mode);
        outFit.Store(FIT_SCALE, asuint(max(0.6, 2.5 * tau)));
        frameState.Store(RST_MOTION_FIT, asuint(float(mode)));
    }
}
