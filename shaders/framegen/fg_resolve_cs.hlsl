// Interpolate stage 2: decode the splatted keys into the motion field at time t, and fill holes.
// A cell with a key gets the 0->1 motion of its winning source cell and that cell's confidence. A hole takes the
// highest-key neighbour within radius 1, then 2 (confidence halved); if none exists, zero motion with confidence 0.
//   gDstSize cell grid size
// Output: float4(0->1 motion in frame px, confidence, valid).
#include "framegen/fg_common.hlsli"

Texture2DArray<float4> gField : register(t0);
Texture2D<uint> gKeys : register(t1);
RWTexture2D<float4> gOut : register(u0);

float4 Decode(uint key)
{
    const uint dir = (key >> 21) & 1u;
    const uint idx = key & 0x1FFFFFu;
    const int2 src = int2(idx % uint(gDstSize.x), idx / uint(gDstSize.x));
    const float4 f = gField.Load(int4(src, dir, 0));
    return float4(dir == 0 ? f.xy : -f.xy, f.z, 1.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 cell = int2(id.xy);
    if (any(cell >= gDstSize)) return;
    uint key = gKeys.Load(int3(cell, 0));
    float scale = 1.0;
    [loop] for (int rad = 1; rad <= 2 && key == 0; ++rad) {
        for (int y = -rad; y <= rad; ++y) {
            for (int x = -rad; x <= rad; ++x) {
                const int2 c = cell + int2(x, y);
                if (any(c < 0) || any(c >= gDstSize)) continue;
                key = max(key, gKeys.Load(int3(c, 0)));
            }
        }
        scale = 0.5;
    }
    float4 o = float4(0, 0, 0, 0);
    if (key != 0) {
        o = Decode(key);
        o.z *= scale;
    }
    gOut[cell] = o;
}
