// Plane-plus-parallax fit shared by the GPU fit and fusion passes. Coordinates are
// centred and normalised by half the capture size so the normal equations stay conditioned.
#ifndef RELIEF_MOTION_HLSLI
#define RELIEF_MOTION_HLSLI

float2 MotionCellPosition(uint2 cell)
{
    return min((float2(cell) + 0.5) / cCellInv, float2(cSize) - 0.5);
}

float2 MotionNormalPosition(float2 position)
{
    return 2.0 * position / float2(cSize) - 1.0;
}

float2 PlaneFlow(float2 p, float3 hx, float3 hy, float2 hw)
{
    const float3 q = float3(p, 1.0);
    const float w = dot(hw, p);
    return (float2(dot(hx, q), dot(hy, q)) - w * p) / max(0.25, 1.0 + w) * (0.5 * float2(cSize));
}

#define FIT_X       0  // float3, H first row minus identity
#define FIT_Y      12  // float3, H second row minus identity
#define FIT_W      24  // float2, projective row
#define FIT_AXIS   32  // float2, epipole in pixels or unit translation axis
#define FIT_MODE   40  // 0 = no detectable translation, 1 = epipole, 2 = parallel, 3 = unusable fit
#define FIT_SCALE  44  // residual reference scale in pixels
#define FIT_BYTES  48

#endif
