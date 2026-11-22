// Regional luma histograms of one frame for scene-cut detection: the image is split into 3x3 regions, each with a
// 16-bin histogram of luma (0..255). Computed once per real frame from a small pyramid level and kept with the
// frame, so a pair compares two stored histograms (never recomputed).
//   gSrcSize  size of the pyramid level read
// Output: 144 counters (region * 16 + bin). The buffer is cleared by the CPU before this pass.
#include "framegen/fg_common.hlsli"

Texture2D<float> gLum : register(t0);
RWBuffer<uint> gHist : register(u0);

groupshared uint sHist[144];

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi < 144) sHist[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (all(int2(id.xy) < gSrcSize)) {
        const float l = gLum.Load(int3(id.xy, 0));
        const uint rx = min(2u, id.x * 3u / uint(gSrcSize.x));
        const uint ry = min(2u, id.y * 3u / uint(gSrcSize.y));
        const uint bin = min(15u, uint(max(l, 0.0) * (16.0 / 256.0)));
        InterlockedAdd(sHist[(ry * 3u + rx) * 16u + bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 144 && sHist[gi] != 0) InterlockedAdd(gHist[gi], sHist[gi]);
}
