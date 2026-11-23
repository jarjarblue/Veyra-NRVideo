// SPDX-License-Identifier: GPL-3.0-only
#include "CapturePredictionCommon.hlsli"
RWTexture2D<float4> VelocityOutput : register(u0);
RWTexture2D<float> ReliabilityOutput : register(u1);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= FlowWidth || id.y >= FlowHeight) return;
    const float2 uv = (float2(id.xy) + .5) / float2(FlowWidth, FlowHeight);
    const float2 scale = float2(Width,Height) / float2(FlowWidth,FlowHeight);
    float2 back = Motion[id.xy];
    float confidence = Confidence[id.xy];
    float2 forward = 0, acceleration = 0;
    bool valid = HistoryValid != 0 && all(isfinite(back)) && isfinite(confidence) &&
        confidence >= .6 && inside(uv + back / float2(FlowWidth,FlowHeight));
    if (valid) {
        float e = mismatch(uv,back);
        valid = isfinite(e);
        confidence *= 1 - smoothstep(.015,.075,e);
        forward = -back * scale;
        float spatial = 0;
        const int2 offsets[4] = {int2(1,0),int2(-1,0),int2(0,1),int2(0,-1)};
        [unroll] for (uint k = 0; k < 4; ++k) {
            float2 v = Motion[clamp(int2(id.xy)+offsets[k],int2(0,0),int2(FlowWidth,FlowHeight)-1)];
            if (!all(isfinite(v))) valid = false;
            else spatial = max(spatial,length((v-back)*scale));
        }
        // At conflicting motion boundaries, prefer the latest real image.
        confidence *= 1 - smoothstep(2,12,spatial);
        if (AccelerationValid != 0) {
            float2 old = PreviousMotion.SampleLevel(LinearClamp,uv+back/float2(FlowWidth,FlowHeight),0);
            if (all(isfinite(old))) {
                acceleration = forward + old * scale * IntervalRatio;
                float magnitude = length(acceleration);
                float limit = min(4.0,length(forward)*.35);
                acceleration *= min(1.0,limit/max(magnitude,1e-5));
                // Sudden reversals/acceleration invalidate the forecast locally.
                confidence *= 1 - smoothstep(4,16,magnitude);
            } else confidence = 0;
        }
    }
    if (!valid || !all(isfinite(forward)) || !all(isfinite(acceleration))) {
        forward = 0; acceleration = 0; confidence = 0;
    }
    VelocityOutput[id.xy] = float4(forward,acceleration);
    ReliabilityOutput[id.xy] = saturate(confidence);
}
