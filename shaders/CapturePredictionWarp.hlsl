// SPDX-License-Identifier: GPL-3.0-only
#include "CapturePredictionCommon.hlsli"
RWTexture2D<float4> Output : register(u0);
float2 displacement(float2 uv, float protection) {
    float4 v = Velocity.SampleLevel(LinearClamp,uv,0);
    float trust = Reliability.SampleLevel(LinearClamp,uv,0);
    float alpha = Horizon * smoothstep(.6,.95,trust) * (1-protection);
    float2 d = v.xy*alpha + .5*Acceleration*v.zw*alpha*(alpha+1);
    d *= min(1.0,MaxDisplacement/max(length(d),1e-5));
    return d;
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Width || id.y >= Height) return;
    float4 latest = Current[id.xy];
    if (HistoryValid == 0 || Horizon <= 0 || SceneGate[uint2(0,0)] < .5) { Output[id.xy] = latest; return; }
    float2 uv = (float2(id.xy)+.5)/float2(Width,Height);
    float protection = NrProtection(float2(id.xy)+.5,uint2(Width,Height),1,Feather,Protected);
    if (protection >= .999) { Output[id.xy] = latest; return; }
    float2 at = uv;
    // Invert the forward field. A single backward sample tears at boundaries.
    [unroll] for (uint k = 0; k < 3; ++k)
        at = uv - displacement(at,protection)/float2(Width,Height);
    bool valid = inside(at);
    float residual = length((at + displacement(at,protection)/float2(Width,Height) - uv)*float2(Width,Height));
    float trust = Reliability.SampleLevel(LinearClamp,at,0);
    // Do not pull HUD pixels out of a protected region into surrounding video.
    float sourceProtection = NrProtection(at*float2(Width,Height),uint2(Width,Height),1,Feather,Protected);
    valid = valid && isfinite(residual) && residual < .75 && trust >= .6 && sourceProtection <= protection+.01;
    float4 predicted = valid ? Current.SampleLevel(LinearClamp,at,0) : latest;
    Output[id.xy] = all(isfinite(predicted)) ? predicted : latest;
}
