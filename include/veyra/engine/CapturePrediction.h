// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace veyra::engine {
// Prediction does not measure HDMI/card delay. Compensation is an explicit
// estimate supplied by the user, never reported as latency removed.
struct CapturePredictionSettings {
    bool enabled = false;
    double compensationMs = 8;
    double maxHorizonMs = 25;
    double maxIntervalFraction = .75;
    double maxGpuMs = 2;
    double displayPeriodMs = 1000.0 / 120;
};
enum class PredictionReason { Disabled, UnsupportedSource, ProviderOwned, Warmup,
    Discontinuity, Duplicate, InvalidClock, Stale, Budget, Ready };
struct PredictionDecision {
    PredictionReason reason = PredictionReason::Disabled;
    double horizonMs = 0, intervalFraction = 0, intervalRatio = 1;
    bool acceleration = false;
    bool active() const { return reason == PredictionReason::Ready; }
};

// Latest-source state only: no frame queue, future-image wait or deadline wait.
// All host timestamps must use the same steady clock in 100 ns units. Source
// PTS is used for continuity only; it is NEVER subtracted from a host clock.
class CapturePredictionClock {
public:
    struct Source {
        uint64_t epoch = 0, revision = 0, id = 0;
        int64_t pts100ns = 0, arrival100ns = 0;
        bool liveCapture = true, providerOwned = false, reset = false;
    };
    void clear() { *this = {}; }
    PredictionReason accept(Source source) {
        if (source.arrival100ns <= 0 || source.pts100ns < 0 || !source.id) {
            clear(); return PredictionReason::InvalidClock;
        }
        // A duplicate must not refresh arrival time, history or the stale limit.
        if (valid_ && !source.reset && source.epoch == latest_.epoch &&
            source.revision == latest_.revision && source.id == latest_.id)
            return PredictionReason::Duplicate;
        const bool continuity = valid_ && !source.reset && source.epoch == latest_.epoch &&
            source.revision == latest_.revision && source.id > latest_.id && source.id - latest_.id == 1 &&
            source.pts100ns > latest_.pts100ns && source.arrival100ns > latest_.arrival100ns;
        const double delta = continuity ? double(source.pts100ns - latest_.pts100ns) / 10000 : 0;
        // Cadence changes/long gaps restart motion history; no old acceleration.
        const bool sane = continuity && delta >= 4 && delta <= 100 &&
            (pairs_ == 0 || std::abs(delta - intervalMs_) <= intervalMs_ * .25);
        previousIntervalMs_ = sane ? intervalMs_ : 0;
        intervalMs_ = sane ? delta : 0;
        pairs_ = sane ? std::min(pairs_ + 1, 3u) : 0;
        latest_ = source; valid_ = true;
        if (cooldown_) --cooldown_;
        return sane ? PredictionReason::Ready : PredictionReason::Discontinuity;
    }
    // Completed GPU timestamps only; never wait for a query/read back pixels.
    void gpuCost(double milliseconds) {
        if (!std::isfinite(milliseconds) || milliseconds < 0) return;
        costs_[costAt_++ % costs_.size()] = milliseconds;
        costCount_ = std::min(costCount_ + 1, unsigned(costs_.size()));
        lastCostMs_ = milliseconds;
    }
    PredictionDecision decide(const CapturePredictionSettings& settings, int64_t now100ns) {
        auto stop = [](PredictionReason reason) { PredictionDecision d; d.reason = reason; return d; };
        if (!settings.enabled) return stop(PredictionReason::Disabled);
        if (!valid_ || pairs_ < 1) return stop(PredictionReason::Warmup);
        if (!latest_.liveCapture) return stop(PredictionReason::UnsupportedSource);
        if (latest_.providerOwned) return stop(PredictionReason::ProviderOwned);
        if (now100ns < latest_.arrival100ns) return stop(PredictionReason::InvalidClock);
        const double age = double(now100ns - latest_.arrival100ns) / 10000;
        if (age >= intervalMs_) return stop(PredictionReason::Stale);
        if (!std::isfinite(settings.compensationMs) || !std::isfinite(settings.maxHorizonMs) ||
            !std::isfinite(settings.maxIntervalFraction) || !std::isfinite(settings.maxGpuMs) ||
            !std::isfinite(settings.displayPeriodMs) || settings.compensationMs < 0 ||
            settings.maxHorizonMs <= 0 || settings.maxIntervalFraction <= 0 ||
            settings.maxGpuMs <= 0 || settings.displayPeriodMs <= 0)
            return stop(PredictionReason::Disabled);
        const double budget = std::min(settings.maxGpuMs, settings.displayPeriodMs * .3);
        if (cooldown_) return stop(PredictionReason::Budget);
        if (costCount_) {
            auto ordered = costs_;
            std::sort(ordered.begin(), ordered.begin() + costCount_);
            const double p95 = ordered[(costCount_ * 95 + 99) / 100 - 1];
            if (lastCostMs_ > budget || (costCount_ >= 8 && p95 > budget)) {
                cooldown_ = 8; costCount_ = costAt_ = 0; lastCostMs_ = 0;
                return stop(PredictionReason::Budget);
            }
        }
        const double limit = std::min(std::clamp(settings.maxHorizonMs, .1, 50.0),
            intervalMs_ * std::clamp(settings.maxIntervalFraction, .01, 1.0));
        PredictionDecision d;
        d.reason = PredictionReason::Ready;
        d.horizonMs = std::min(limit, settings.compensationMs + age + std::min(lastCostMs_, budget));
        d.intervalFraction = d.horizonMs / intervalMs_;
        d.acceleration = pairs_ >= 2;
        d.intervalRatio = previousIntervalMs_ > 0 ? intervalMs_ / previousIntervalMs_ : 1;
        return d;
    }
private:
    Source latest_{};
    bool valid_ = false;
    unsigned pairs_ = 0, cooldown_ = 0, costCount_ = 0, costAt_ = 0;
    double intervalMs_ = 0, previousIntervalMs_ = 0, lastCostMs_ = 0;
    std::array<double, 48> costs_{};
};
}
