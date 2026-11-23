// SPDX-License-Identifier: GPL-3.0-only
#include "NrProtection.hlsli"
Texture2D<float4> Current : register(t0);
Texture2D<float4> Previous : register(t1);
Texture2D<float2> Motion : register(t2);
Texture2D<float2> PreviousMotion : register(t3);
Texture2D<float> Confidence : register(t4);
Texture2D<float4> Velocity : register(t5);
Texture2D<float> Reliability : register(t6);
Texture2D<float> SceneGate : register(t7);
SamplerState LinearClamp : register(s1);
cbuffer PredictionConstants : register(b0) {
    uint Width, Height, FlowWidth, FlowHeight;
    float Horizon, IntervalRatio, MaxDisplacement, Acceleration;
    uint HistoryValid, AccelerationValid;
    float Feather, Padding;
    float4 Protected[4];
};
float perceptual(float3 c) {
    // Log-luma is finite over SDR and scene-linear HDR, no output transfer here.
    return log2(1 + dot(max(c, 0), float3(.2126, .7152, .0722)));
}
bool inside(float2 uv) {
    const float2 halfTexel = .5 / float2(Width, Height);
    return all(isfinite(uv)) && all(uv >= halfTexel) && all(uv <= 1 - halfTexel);
}
float mismatch(float2 uv, float2 backward) {
    float error = 0;
    // Cross-patch photometric check, not just a single ambiguous pixel.
    const float2 offsets[5] = {float2(0,0), float2(1,0),float2(-1,0),float2(0,1),float2(0,-1)};
    [unroll] for (uint k = 0; k < 5; ++k) {
        float2 delta = offsets[k] / float2(Width, Height);
        float a = perceptual(Current.SampleLevel(LinearClamp, uv + delta, 0).rgb);
        float b = perceptual(Previous.SampleLevel(LinearClamp, uv + backward / float2(FlowWidth, FlowHeight) + delta, 0).rgb);
        error += abs(a - b);
    }
    return error * .2;
}
