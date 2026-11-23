// SPDX-License-Identifier: GPL-3.0-only
#include "veyra/pipeline/CapturePredictionPass.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>

namespace veyra::pipeline {
namespace {
float bits(uint32_t value) { return std::bit_cast<float>(value); }
bool extent(ID3D12Resource* r, uint32_t width, uint32_t height) {
    if (!r) return false;
    const auto d = r->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == width &&
        d.Height == height && d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.SampleDesc.Count == 1 &&
        !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}
}
bool CapturePredictionPass::initialize(ID3D12Device* device, Inputs in,
                                      const std::vector<ID3D12Resource*>& outputs) {
    close();
    if (!device || !in.current || !in.motion || outputs.empty() || outputs.size() > 8) return false;
    const auto color = in.current->GetDesc(), flow = in.motion->GetDesc();
    if (color.Width > UINT32_MAX || flow.Width > UINT32_MAX || !color.Width || !color.Height || !flow.Width || !flow.Height) return false;
    width_ = uint32_t(color.Width); height_ = color.Height;
    flowWidth_ = uint32_t(flow.Width); flowHeight_ = flow.Height;
    if (color.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || !extent(in.current,width_,height_) ||
        !extent(in.previous,width_,height_) || in.previous->GetDesc().Format != color.Format ||
        !extent(in.motion,flowWidth_,flowHeight_) || !extent(in.previousMotion,flowWidth_,flowHeight_) ||
        in.previousMotion->GetDesc().Format != flow.Format ||
        (flow.Format != DXGI_FORMAT_R16G16_FLOAT && flow.Format != DXGI_FORMAT_R32G32_FLOAT) ||
        !extent(in.confidence,flowWidth_,flowHeight_)) return false;
    const auto confidenceFormat = in.confidence->GetDesc().Format;
    if (confidenceFormat != DXGI_FORMAT_R16_FLOAT && confidenceFormat != DXGI_FORMAT_R32_FLOAT &&
        confidenceFormat != DXGI_FORMAT_R8_UNORM) return false;
    std::set<ID3D12Resource*> unique;
    for (auto* r : {in.current,in.previous,in.motion,in.previousMotion,in.confidence})
        if (!unique.insert(r).second) return false;
    for (auto* r : outputs) {
        if (!extent(r,width_,height_) || r->GetDesc().Format != color.Format ||
            !(r->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !unique.insert(r).second) return false;
    }
    inputs_ = {in.current,in.previous,in.motion,in.previousMotion,in.confidence};
    for (auto* output : outputs) outputs_.emplace_back(output);
    velocity_ = makeTexture(device,flowWidth_,flowHeight_,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
    reliability_ = makeTexture(device,flowWidth_,flowHeight_,DXGI_FORMAT_R16_FLOAT,true);
    gate_ = makeTexture(device,1,1,DXGI_FORMAT_R32_FLOAT,true);
    if (!velocity_ || !reliability_ || !gate_) { close(); return false; }
    for (auto* r : {velocity_.Get(),reliability_.Get(),gate_.Get()}) {
        auto d = r->GetDesc(); bytes_ += device->GetResourceAllocationInfo(0,1,&d).SizeInBytes;
    }
    std::vector<uint8_t> shader;
    if (!scene_.loadShader("CapturePredictionScene.dxil",shader) || !scene_.create(device,shader,9,8,1,28,0,true) ||
        !prepare_.loadShader("CapturePredictionPrepare.dxil",shader) || !prepare_.create(device,shader,10,8,2,28,0,true) ||
        !warp_.loadShader("CapturePredictionWarp.dxil",shader) || !warp_.create(device,shader,UINT(outputs.size()*9),8,1,28,0,true)) {
        close(); return false;
    }
    DescriptorStager stage;
    if (!stage.initialize(device,32)) { close(); return false; }
    auto views = [&](ComputePass& pass, unsigned offset) {
        for (unsigned i = 0; i < inputs_.size(); ++i) stage.stageSrv(inputs_[i].Get(),nullptr,pass.heap.Get(),offset+i);
        stage.stageSrv(velocity_.Get(),nullptr,pass.heap.Get(),offset+5);
        stage.stageSrv(reliability_.Get(),nullptr,pass.heap.Get(),offset+6);
        stage.stageSrv(gate_.Get(),nullptr,pass.heap.Get(),offset+7);
    };
    views(scene_,0); views(prepare_,0);
    makeUav(device,gate_.Get(),DXGI_FORMAT_R32_FLOAT,cpuHandleOf(scene_,8));
    makeUav(device,velocity_.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,cpuHandleOf(prepare_,8));
    makeUav(device,reliability_.Get(),DXGI_FORMAT_R16_FLOAT,cpuHandleOf(prepare_,9));
    for (unsigned i = 0; i < outputs_.size(); ++i) {
        views(warp_,i*9);
        makeUav(device,outputs_[i].Get(),color.Format,cpuHandleOf(warp_,i*9+8));
    }
    return true;
}
bool CapturePredictionPass::record(ID3D12GraphicsCommandList* list, StateTracker& tracker,
                                   unsigned index, const Parameters& p) {
    if (!list || !warp_.pso || index >= outputs_.size() ||
        !std::isfinite(p.horizonFraction) || p.horizonFraction < 0 || p.horizonFraction > 1 ||
        !std::isfinite(p.intervalRatio) || p.intervalRatio < .5f || p.intervalRatio > 2 ||
        !std::isfinite(p.maxDisplacementPixels) || p.maxDisplacementPixels < 0 || p.maxDisplacementPixels > 128 ||
        !std::isfinite(p.accelerationStrength) || p.accelerationStrength < 0 || p.accelerationStrength > 1 ||
        !std::isfinite(p.protectionFeatherPixels) || p.protectionFeatherPixels < 0) return false;
    for (const auto& r : p.protectedRegions) for (float v : r) if (!std::isfinite(v) || v < 0 || v > 1) return false;
    auto* output = outputs_[index].Get();
    // Exact passthrough, including alpha/HDR values. No history reads on reset.
    if (!p.historyValid || p.horizonFraction == 0) {
        tracker.transition(list,inputs_[0].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);
        tracker.transition(list,output,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(output,inputs_[0].Get());
        tracker.transition(list,inputs_[0].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        tracker.transition(list,output,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    }
    float c[28] = {bits(width_),bits(height_),bits(flowWidth_),bits(flowHeight_),p.horizonFraction,p.intervalRatio,
        p.maxDisplacementPixels,p.accelerationStrength,bits(1),bits(p.accelerationValid?1u:0u),p.protectionFeatherPixels,0};
    for (unsigned r = 0; r < 4; ++r) std::copy(p.protectedRegions[r].begin(),p.protectedRegions[r].end(),c+12+r*4);
    for (const auto& input : inputs_) tracker.transition(list,input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list,gate_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    scene_.bind(list,c,gpuHandleOf(scene_,0).ptr,gpuHandleOf(scene_,8).ptr);
    list->Dispatch(1,1,1); tracker.uavBarrier(list,gate_.Get());
    tracker.transition(list,gate_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list,velocity_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tracker.transition(list,reliability_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    prepare_.bind(list,c,gpuHandleOf(prepare_,0).ptr,gpuHandleOf(prepare_,8).ptr);
    list->Dispatch((flowWidth_+7)/8,(flowHeight_+7)/8,1);
    tracker.uavBarrier(list,velocity_.Get()); tracker.uavBarrier(list,reliability_.Get());
    tracker.transition(list,velocity_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list,reliability_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    warp_.bind(list,c,gpuHandleOf(warp_,index*9).ptr,gpuHandleOf(warp_,index*9+8).ptr);
    list->Dispatch((width_+7)/8,(height_+7)/8,1); tracker.uavBarrier(list,output);
    tracker.transition(list,output,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}
void CapturePredictionPass::close() {
    scene_={}; prepare_={}; warp_={}; inputs_={}; outputs_.clear();
    velocity_.Reset(); reliability_.Reset(); gate_.Reset();
    width_=height_=flowWidth_=flowHeight_=0; bytes_=0;
}
}
