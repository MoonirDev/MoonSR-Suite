// One brightness-offset-invariant Gauss-Newton step on the 5-tap cross.
// Called only for uncertain but trackable motion. A fresh photometric check must
// improve on the selected cell vector before the correction is used.
void RefinePixel(float2 p, float current[5], float meanCurrent, inout float4 motion, inout float error)
{
    float previous[9];
    [unroll] for (int k = 0; k < 9; ++k)
        previous[k] = LPrev(p + motion.xy + float2(k % 3 - 1, k / 3 - 1));

    const float2 gradient[5] = {
        0.5 * float2(previous[5] - previous[3], previous[7] - previous[1]),
        float2(previous[4] - previous[3], 0.5 * (previous[6] - previous[0])),
        float2(previous[5] - previous[4], 0.5 * (previous[8] - previous[2])),
        float2(0.5 * (previous[2] - previous[0]), previous[4] - previous[1]),
        float2(0.5 * (previous[8] - previous[6]), previous[7] - previous[4])
    };
    const float values[5] = {previous[4], previous[3], previous[5], previous[1], previous[7]};
    const float meanPrevious = (values[0] + values[1] + values[2] + values[3] + values[4]) * 0.2;
    const float2 meanGradient = (gradient[0] + gradient[1] + gradient[2] + gradient[3] + gradient[4]) * 0.2;
    float3 normal = float3(0.0004, 0, 0.0004);
    float2 rhs = 0;
    [unroll] for (int i = 0; i < 5; ++i) {
        const float2 g = gradient[i] - meanGradient;
        normal += float3(g.x * g.x, g.x * g.y, g.y * g.y);
        rhs += g * (current[i] - meanCurrent - values[i] + meanPrevious);
    }
    const float determinant = normal.x * normal.z - normal.y * normal.y;
    if (determinant < 1e-6) return;
    const float2 step = float2(normal.z * rhs.x - normal.y * rhs.y,
                               normal.x * rhs.y - normal.y * rhs.x) / determinant;
    const float2 correction = step * min(1.0, 0.35 / max(length(step), 1e-6));
    if (dot(correction, correction) < 0.0004) return;

    static const float2 offsets[5] = {float2(0, 0), float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1)};
    const float2 candidate = motion.xy + correction;
    const float2 target = p + candidate;
    if (any(target < 0.5) || any(target > gIn.xy - 0.5)) return;
    float taps[5], mean = 0;
    [unroll] for (int j = 0; j < 5; ++j) {
        taps[j] = LPrev(target + offsets[j]);
        mean += 0.2 * taps[j];
    }
    float cost = 0;
    [unroll] for (int k2 = 0; k2 < 5; ++k2)
        cost += abs((current[k2] - meanCurrent) - (taps[k2] - mean));
    cost = 0.2 * cost + 0.5 * abs(meanCurrent - mean);
    if (cost + 0.001 < error) {
        motion.xy = candidate;
        error = cost;
    }
}
