// Conservative sampling evidence. A point label needs persistent concentrated steps.
// Failure to distinguish a sampling model deliberately leaves back-projection in use.
#include "upscale/alsr_common.hlsli"

Texture2D<float4> gSource : register(t0);
Texture2D<float4> gPrevious : register(t1);
Buffer<uint> gStats : register(t2);
Texture2D<float4> gPreviousSource : register(t3);
RWTexture2D<float4> gDecision : register(u0);
#ifdef ALSR_SEED_ARGS
RWByteAddressBuffer gSeedArgs : register(u1);
#endif

static const uint kGridW = 64;
static const uint kGridH = 36;
static const uint kLanes = 128;
groupshared float4 gPartial[kLanes];

float3 EdgeStep(float first, float middle, float last)
{
    const float a = middle - first, b = last - middle;
    const float span = abs(last - first);
    if (span <= 0.10 || a * b < 0.0) return 0;
    const float sharpness = max(abs(a), abs(b)) / span;
    return float3(sharpness, sharpness >= 0.90 ? 1.0 : 0.0, 1.0);
}

float Fetch(int2 p)
{
    return LumaOf(gSource.Load(int3(p, 0)).rgb);
}

[numthreads(128, 1, 1)]
void main(uint lane : SV_GroupIndex)
{
    float4 sum = 0;
    const bool staticEnabled = (gU1.w & (1u << 19)) != 0;
    if (all(gIn.xy >= float2(kGridW + 2, kGridH + 2))) {
        [loop] for (uint cell = lane; cell < kGridW * kGridH; cell += kLanes) {
            const float2 grid = (float2(cell % kGridW, cell / kGridW) + 0.5) / float2(kGridW, kGridH);
            const int2 p = int2(grid * (gIn.xy - 2)) + 1;
            const float c = Fetch(p);
            sum.xyz += EdgeStep(Fetch(p - int2(1, 0)), c, Fetch(p + int2(1, 0)));
            sum.xyz += EdgeStep(Fetch(p - int2(0, 1)), c, Fetch(p + int2(0, 1)));
            if (staticEnabled) {
                const float3 d = abs(gSource.Load(int3(p, 0)).rgb - gPreviousSource.Load(int3(p, 0)).rgb);
                sum.w += any(d > 2.0 / 255.0) ? 1.0 : 0.0;
            }
        }
    }
    gPartial[lane] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = kLanes / 2; step > 0; step /= 2) {
        if (lane < step) gPartial[lane] += gPartial[lane + step];
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane != 0) return;

    const float4 total = gPartial[0];
    const float2 score = total.xy / max(total.z, 1.0);
    const bool enoughEdges = total.z >= 128.0;
    const bool adaptive = (gU1.w & (1u << 20)) != 0;
    // A large population of abrupt steps in a narrow repeating pattern can also
    // come from an area-sampled grid. Require a stronger response in dense scenes.
    const float density = total.z / float(2 * kGridW * kGridH);
    const bool sparse = density <= 0.14 && score.x >= 0.82 && score.y >= 0.38;
    const bool nearBinary = score.x >= 0.975 && score.y >= 0.94;
    bool pointCandidate = enoughEdges && (adaptive ? (sparse || nearBinary) :
                                          (score.x >= 0.95 && score.y >= 0.85));
    const bool areaCandidate = enoughEdges && score.x <= 0.75 && score.y <= 0.20;
    const bool restart = gU0.x == 0 || (gU1.y != 0 && gStats[gU1.x] != 0);
    const float4 previous = restart ? 0.0 : gPrevious.Load(int3(0, 0, 0));
    float3 pooled = 0;
    if ((gU1.w & (1u << 21)) != 0) {
        // Weight frame statistics by the number of edge samples before averaging.
        // A scene cut restarts the measurement and the confirmation streak.
        pooled = restart ? total.xyz : lerp(gPrevious.Load(int3(2, 0, 0)).xyz, total.xyz, 0.20);
        const float2 pooledScore = pooled.xy / max(pooled.z, 1.0);
        const bool confirmed = previous.x == 2.0;
        const bool viable = total.z >= 64.0 && score.x >= 0.77 && score.y >= 0.28;
        const bool sparsePool = pooled.z >= 96.0 && pooled.z <= 0.14 * float(2 * kGridW * kGridH);
        const bool concentrated = pooledScore.x >= (confirmed ? 0.805 : 0.815) &&
                                  pooledScore.y >= (confirmed ? 0.34 : 0.38);
        pointCandidate = (enoughEdges && nearBinary) || (viable && sparsePool && concentrated);
    }
    const float previousStreak = previous.y;
    const float streak = pointCandidate ? min(previousStreak + 1.0, 3.0) : 0.0;
    const float model = streak >= 3.0 ? 2.0 : (areaCandidate ? 1.0 : 0.0);
    const bool supportedGrid = all(gIn.xy >= float2(kGridW + 2, kGridH + 2));
    const bool stillFrame = staticEnabled && supportedGrid && !restart && areaCandidate && total.w <= 2.0;
    const float stillStreak = stillFrame ? min(previous.w + 1.0, 12.0) : 0.0;
    // Attach slowly to a stationary scene. Withdraw over at most two frames after motion returns.
    const bool significantChange = total.w > 0.04 * kGridW * kGridH;
    const float blend = restart || significantChange ? 0.0 : (stillStreak >= 4.0 ? min(previous.z + 0.125, 1.0) :
                        (stillFrame ? previous.z : max(previous.z - 0.5, 0.0)));
    gDecision[int2(0, 0)] = float4(model, streak, blend, stillStreak);
    // Evidence for benchmark readback. Only the first texel feeds later passes.
    const uint flags = (restart ? 2u : 0u) | (pointCandidate ? 1u : 0u);
    gDecision[int2(1, 0)] = float4(total.z, score, float(flags));
    gDecision[int2(2, 0)] = float4(pooled, 0.0);
#ifdef ALSR_SEED_ARGS
    const uint pointInstances = streak >= 3.0 ? 1u : 0u;
    // DrawInstanced arguments: vertices, instances, first vertex, first instance.
    gSeedArgs.Store4(0, uint4(3, 1u - pointInstances, 0, 0));
    gSeedArgs.Store4(16, uint4(3, pointInstances, 0, 0));
#endif
}
