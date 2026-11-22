// Surroundings motion for HUD protection. One thread per cell.
// Over a 9x9 grid of cells sampled with stride gU0.y around the cell: the fraction of cells whose direction-0 vector
// is longer than gP0.x frame px with confidence above gP0.y, and the mean vector of those moving cells.
//   gDstSize cell grid size
// Output: float4(fraction 0..1, mean moving vector in frame px (0 if none), 0).
#include "framegen/fg_common.hlsli"

Texture2DArray<float4> gField : register(t0);
RWTexture2D<float4> gOut : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 cell = int2(id.xy);
    if (any(cell >= gDstSize)) return;
    const int stride = int(gU0.y);
    float moving = 0.0, total = 0.0;
    float2 sumV = float2(0, 0);
    [loop] for (int y = -4; y <= 4; ++y) {
        [loop] for (int x = -4; x <= 4; ++x) {
            const int2 c = cell + int2(x, y) * stride;
            if (any(c < 0) || any(c >= gDstSize)) continue;
            const float4 f = gField.Load(int4(c, 0, 0));
            total += 1.0;
            if (length(f.xy) > gP0.x && f.z > gP0.y) {
                moving += 1.0;
                sumV += f.xy;
            }
        }
    }
    gOut[cell] = float4(total > 0.0 ? moving / total : 0.0, moving > 0.0 ? sumV / moving : float2(0, 0), 0.0);
}
