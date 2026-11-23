// Fuse coarse relative inverse depth from residual parallax in capture-sized cell flow.
// Depth is relative to the fitted plane; its scale is deliberately bounded and scene-local.
#include "relief/relief_common.hlsli"
#include "relief/relief_motion.hlsli"

Texture2DArray<float4> flowTex  : register(t0);
ByteAddressBuffer fit          : register(t1);
Texture2D<float4> prevDepth    : register(t2);
ByteAddressBuffer frameState   : register(t3);
SamplerState linClamp          : register(s0);
RWTexture2D<float4> nextDepth   : register(u0); // inverse depth, confidence, independent motion, support

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= cCells)) return;
    if (asfloat(frameState.Load(RST_CUT)) > 0.5) {
        nextDepth[id.xy] = 0.0;
        return;
    }
    const uint mode = fit.Load(FIT_MODE);
    const float4 flow = flowTex.Load(int4(id.xy, 1, 0));
    const float2 p = MotionCellPosition(id.xy);
    const float2 prev = p + flow.xy;
    const float reliable = smoothstep(0.25, 0.75, flow.z) * (1.0 - smoothstep(0.2, 0.65, flow.w));
    float4 history = 0.0;
    if ((cFlags & RELIEF_F_DEPTH_HISTORY) && all(prev >= 0.0) && all(prev < float2(cSize)))
        history = prevDepth.SampleLevel(linClamp, prev * cCellInv / float2(cCells), 0);
    history.yw *= reliable;
    if (mode == 0u || mode == 3u) {
        // Pure rotation and still cameras provide no new depth. An unusable fit
        // fades history quickly; a sound rotation fit keeps its reprojected depth.
        history.yw *= mode == 0u ? 0.995 : 0.7;
        history.z *= 0.8;
        nextDepth[id.xy] = history;
        return;
    }

    const float2 plane = PlaneFlow(MotionNormalPosition(p), asfloat(fit.Load3(FIT_X)),
                                   asfloat(fit.Load3(FIT_Y)), asfloat(fit.Load2(FIT_W)));
    const float2 residual = flow.xy - plane;
    const float2 axis = asfloat(fit.Load2(FIT_AXIS));
    const float scale = asfloat(fit.Load(FIT_SCALE));
    float2 direction = axis;
    float foreshortening = 1.0;
    if (mode == 1u) {
        const float2 radial = p - axis;
        const float distance = length(radial);
        direction = radial / max(distance, 1.0);
        // Signed inverse depth relative to the plane is residual / epipole distance.
        // Clamp near the epipole, where even perfect flow carries almost no scale information.
        foreshortening = min(3.0, 0.45 * length(float2(cSize)) / max(distance, 1.0));
        const float2 centreRadial = 0.5 * float2(cSize) - axis;
        const float2 centreFlow = PlaneFlow(0.0, asfloat(fit.Load3(FIT_X)), asfloat(fit.Load3(FIT_Y)),
                                            asfloat(fit.Load2(FIT_W)));
        if (dot(centreFlow, centreRadial) < 0.0) direction = -direction;
    }
    const float along = dot(residual, direction);
    const float cross = abs(dot(residual, float2(-direction.y, direction.x)));
    const float inconsistent = smoothstep(0.45, 1.6, cross) *
                               smoothstep(0.2, 0.65, cross / max(length(residual), 1e-4));
    const float confidence = reliable * (1.0 - inconsistent);
    const float measured = clamp(along * foreshortening / scale, -1.0, 1.0);
    const float oldWeight = 0.7 * history.y;
    const float newWeight = confidence * 0.45;
    const float depth = (oldWeight * history.x + newWeight * measured) / max(oldWeight + newWeight, 1e-4);
    nextDepth[id.xy] = float4(depth, max(confidence, 0.85 * history.y),
                             max(inconsistent * reliable, 0.8 * history.z), max(reliable, 0.85 * history.w));
}
