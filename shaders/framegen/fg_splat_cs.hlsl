// Interpolate stage 1: forward-splat both cell fields to time t (per generated frame, cheap: cell resolution).
//
// A direction-0 cell at frame position x (cell centre) with vector v (prev -> curr) is at x + t v at time t; a
// direction-1 cell at x with vector w (curr -> prev) is at x + (1 - t) w and has 0->1 motion -w. The cell writes a
// 32-bit key into every target cell whose centre lies within 0.75 cell of its landing point (1-2 cells per axis,
// so stretched areas do not open holes). Collisions are resolved without depth by InterlockedMax on
//     key = (1 + confidence * 1022) << 22 | direction << 21 | source cell index
// i.e. the more trustworthy motion wins. Confidence already contains the forward-backward check, so the occluded
// side of a motion boundary (inconsistent) loses against the occluding side (consistent).
//   gDstSize cell grid size   gP0 = (t, cell size in frame px, 1 / cell size, 0)
#include "framegen/fg_common.hlsli"

Texture2DArray<float4> gField : register(t0);
RWTexture2D<uint> gKeys : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 cell = int2(id.xy);
    const uint dir = id.z;
    if (any(cell >= gDstSize)) return;
    const float4 f = gField.Load(int4(cell, dir, 0));
    const float t = gP0.x;
    const float cs = gP0.y;
    const float2 x = (float2(cell) + 0.5) * cs;
    const float2 land = x + (dir == 0 ? t : 1.0 - t) * f.xy;
    const uint q = 1u + uint(saturate(f.z) * 1022.0 + 0.5);
    const uint key = (q << 22) | (dir << 21) | (uint(cell.y * gDstSize.x + cell.x) & 0x1FFFFFu);

    const float2 lc = land * gP0.z - 0.5; // landing point in cell-index units
    const int2 lo = int2(ceil(lc - 0.75));
    const int2 hi = int2(floor(lc + 0.75));
    for (int y = lo.y; y <= hi.y; ++y) {
        for (int xx = lo.x; xx <= hi.x; ++xx) {
            if (xx < 0 || y < 0 || xx >= gDstSize.x || y >= gDstSize.y) continue;
            InterlockedMax(gKeys[int2(xx, y)], key);
        }
    }
}
