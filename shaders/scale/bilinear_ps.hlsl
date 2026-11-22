// Bilinear scaling. 1 fetch. Baseline / fallback scaler.
#include "common/scale_common.hlsli"

float4 main(VsOut i) : SV_Target
{
    return float4(SampleSrcLinear(SrcPosTexels(i.uv)), 1.0);
}
