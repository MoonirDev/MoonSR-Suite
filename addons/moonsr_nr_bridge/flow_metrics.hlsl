// SPDX-License-Identifier: GPL-3.0-only
// Intermediate field compatible with software refinement: displacement (frame px), RMS residual, source stddev.
cbuffer Parameters : register(b0) { uint2 frameSize; uint2 gridSize; uint direction; uint3 reserved; };
Texture2D<int2> vectors : register(t0);
Texture2D<float4> previousFrame : register(t1);
Texture2D<float4> currentFrame : register(t2);
SamplerState linearClamp : register(s0);
RWTexture2D<float4> field : register(u0);
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)) * 255.0; }
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gridSize)) return;
    const float2 v = float2(vectors.Load(int3(id.xy,0))) / 32.0;
    const int2 origin = int2(id.xy) * 4 - 1;
    float sd = 0, sd2 = 0, sum = 0, sum2 = 0;
    [loop] for (int y = 0; y < 6; ++y) {
        [unroll] for (int x = 0; x < 6; ++x) {
            const int2 p = clamp(origin + int2(x,y), 0, int2(frameSize)-1);
            // Clamp only the load: displaced UVs use the original window coordinates, matching software refinement.
            const float2 uv = (float2(origin + int2(x,y)) + .5 + v) / float2(frameSize);
            float a, b;
            if (direction == 0) {
                a = Luma(previousFrame.Load(int3(p,0)).rgb);
                b = Luma(currentFrame.SampleLevel(linearClamp,uv,0).rgb);
            } else {
                a = Luma(currentFrame.Load(int3(p,0)).rgb);
                b = Luma(previousFrame.SampleLevel(linearClamp,uv,0).rgb);
            }
            const float d = a-b;
            sd += d; sd2 += d*d; sum += a; sum2 += a*a;
        }
    }
    field[id.xy] = float4(v, sqrt(max(sd2-sd*sd/36.0,0.0)/36.0),
                         sqrt(max(sum2/36.0-(sum/36.0)*(sum/36.0),0.0)));
}
