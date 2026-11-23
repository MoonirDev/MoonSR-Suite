// Smart parallax analysis, pass 1: prefiltered log2 luminance (full game resolution). Owner: upscaling-engineer.
// L = log2(binomial5x5(Y_linear) + eps), stored encoded (LEnc). The 5x5 binomial suppresses aliasing and single-pixel
// noise, which would otherwise become crawling relief (research note 1.2 step 1 and 3.6). The filter is separable:
// horizontal 5-tap sums of the tile rows in group shared memory, then the vertical 5 taps. Frame statistics are taken
// in the half-size gate pass, so this pass has no reduction and no atomics.
#include "relief/relief_common.hlsli"

Texture2D<float4>        srcTex : register(t0); // game image (gamma-encoded SDR)
RWTexture2D<unorm float> outL   : register(u0); // encoded L (LEnc)

#define TILE 16
#define HALO 2
#define SIDE (TILE + 2 * HALO)
groupshared float gsY[SIDE * SIDE];
groupshared float gsH[SIDE * TILE]; // horizontal binomial sums: all SIDE rows, TILE columns

float LoadY(int2 p)
{
    p = clamp(p, int2(0, 0), int2(cSize) - 1);
    const float3 c = saturate(srcTex.Load(int3(p + cSrcOffset, 0)).rgb);
    return dot(SrgbToLinear3(c), kRec709);
}

[numthreads(TILE, TILE, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const int2 base = int2(gid.xy) * TILE - HALO;
    for (uint i = gi; i < SIDE * SIDE; i += TILE * TILE)
        gsY[i] = LoadY(base + int2(i % SIDE, i / SIDE));
    GroupMemoryBarrierWithGroupSync();
    for (uint j = gi; j < SIDE * TILE; j += TILE * TILE) {
        const uint row = j / TILE, col = j % TILE;
        const uint k = row * SIDE + col;
        gsH[j] = gsY[k] + 4.0 * gsY[k + 1] + 6.0 * gsY[k + 2] + 4.0 * gsY[k + 3] + gsY[k + 4];
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 p = int2(gid.xy) * TILE + int2(gtid.xy);
    if (any(p >= int2(cSize))) return;
    const uint k = gtid.y * TILE + gtid.x;
    const float y = (gsH[k] + 4.0 * gsH[k + TILE] + 6.0 * gsH[k + 2 * TILE] + 4.0 * gsH[k + 3 * TILE] + gsH[k + 4 * TILE]) *
                    (1.0 / 256.0);
    outL[p] = LEnc(log2(y + kLogEps));
}
