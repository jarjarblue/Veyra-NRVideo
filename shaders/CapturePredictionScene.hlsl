// SPDX-License-Identifier: GPL-3.0-only
#include "CapturePredictionCommon.hlsli"
RWTexture2D<float> GateOutput : register(u0);
groupshared uint rejected[64];
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_GroupThreadID, uint index : SV_GroupIndex) {
    uint bad = 0;
    // Fixed 64x36 stratified screen coverage, evaluated entirely on the GPU.
    for (uint k = index; k < 2304; k += 64) {
        float2 uv = (float2(k % 64, k / 64) + .5) / float2(64,36);
        float2 v = Motion.SampleLevel(LinearClamp, uv, 0);
        float confidence = Confidence.SampleLevel(LinearClamp, uv, 0);
        bool valid = HistoryValid != 0 && all(isfinite(v)) && isfinite(confidence) && confidence >= .6;
        if (valid) valid = inside(uv + v / float2(FlowWidth, FlowHeight));
        if (valid) { float e = mismatch(uv, v); valid = isfinite(e) && e < .08; }
        bad += valid ? 0 : 1;
    }
    rejected[index] = bad;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 32; stride > 0; stride >>= 1) {
        if (index < stride) rejected[index] += rejected[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) GateOutput[uint2(0,0)] = rejected[0] < 1267 ? 1 : 0;
}
