// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "veyra/pipeline/GpuPassUtils.h"
#include <array>

namespace veyra::pipeline {
// Development core, not yet connected to the production presenter.
// GPU-only three-pass extrapolation. Inputs are observed images/estimated
// motion, never game-engine vectors or knowledge of future controller input.
class CapturePredictionPass {
public:
    struct Inputs {
        ID3D12Resource* current = nullptr; // scene-linear RGBA16F, full extent
        ID3D12Resource* previous = nullptr;
        ID3D12Resource* motion = nullptr; // current->previous, FLOW-extent pixels
        ID3D12Resource* previousMotion = nullptr;
        ID3D12Resource* confidence = nullptr; // normalized 0..1, flow extent
    };
    struct Parameters {
        float horizonFraction = 0, intervalRatio = 1;
        float maxDisplacementPixels = 48, accelerationStrength = .25f;
        bool historyValid = false, accelerationValid = false;
        float protectionFeatherPixels = 4;
        std::array<std::array<float, 4>, 4> protectedRegions{};
    };
    bool initialize(ID3D12Device*, Inputs, const std::vector<ID3D12Resource*>& outputs);
    // All resources must be retired by the caller's existing slot/fence ring.
    // No submit, queue, CPU/GPU wait, pixel readback or dynamic descriptor edits.
    bool record(ID3D12GraphicsCommandList*, StateTracker&, unsigned output, const Parameters&);
    uint64_t bytes() const { return bytes_; }
    void close(); // caller drains its GPU queue before destroy/reinitialize
private:
    ComputePass scene_, prepare_, warp_;
    std::array<ComPtr<ID3D12Resource>, 5> inputs_;
    std::vector<ComPtr<ID3D12Resource>> outputs_;
    ComPtr<ID3D12Resource> velocity_, reliability_, gate_;
    uint32_t width_ = 0, height_ = 0, flowWidth_ = 0, flowHeight_ = 0;
    uint64_t bytes_ = 0;
};
}
