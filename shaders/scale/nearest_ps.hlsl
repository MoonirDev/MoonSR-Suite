// Nearest-neighbour scaling. 1 fetch. Used for integer scaling and pixel art.
#include "common/scale_common.hlsli"

float4 main(VsOut i) : SV_Target
{
    return float4(LoadSrc(int2(floor(SrcPosTexels(i.uv)))), 1.0);
}
