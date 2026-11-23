# GPU capture prediction: first development milestone

Target: PS5 HDMI capture viewed on an RX 7900 XT, currently using 30 Hz content
and XeSS 4x to a 120 Hz monitor. User requires **no added presentation buffering**.

This milestone implements a standalone prediction controller, three GPU compute
passes, and synthetic truth tests. **It is not wired into the player, its source
selection, presentation loop, settings, or existing XeSS mode. No new player EXE
or user-facing negative-delay switch is delivered by this milestone.**

## Meaning of compensation

Extrapolation can advance the apparent position of already observed motion. It
does not recover unobserved controller actions, reveal unseen geometry, alter PS5
rendering, or prove physical input-to-photon latency reduction. HDMI/card delay
cannot be measured by subtracting a source PTS from the PC clock. The requested
compensation is an estimate; only time since application ingress is observed.

## Implemented core

`CapturePredictionClock` stores one source identity and bounded scalar timing
history. It never stores a video queue or waits for a future source. A duplicate
does not refresh its age. Epoch/revision changes, explicit reset, dropped IDs,
nonmonotonic PTS/arrival, and cadence discontinuity invalidate motion history.
Stale inputs stop prediction after one source interval. Requested forecast
length is capped by both milliseconds and a fraction of the source period.
Defaults are disabled, 8 ms estimated compensation and a 25 ms total horizon,
capped at 0.75 source intervals. Completed GPU timestamps can impose a 2 ms /
30% refresh-budget limit, with an eight-input cooldown and bounded retry.

`CapturePredictionPass` accepts scene-linear RGBA16F source/history, estimated
current-to-previous motion in **flow-resolution pixel units**, prior motion,
and normalized confidence. It records work on the caller's command list:

1. Scene gate: stratified 64x36 motion-compensated photometric tests and a
   GPU group reduction. A majority of invalid correspondences bypasses forecast.
2. Motion preparation: cross-patch photometric validation, spatial motion-boundary
   attenuation, interval-normalized acceleration with magnitude limits, NaN and
   domain checks. Confidence bounds local forecast strength.
3. Warp: three fixed-point inverse iterations, bounded displacement, mapping
   residual validation, border/uncertain-pixel fallback to the latest real image,
   and four explicit HUD protection regions. It does not invent unseen pixels.

CPU-side reset/zero horizon is a bit-exact GPU copy. No normal-path pixel
readback, fence wait, provider patch, submission thread or descriptor mutation
is introduced. Output textures are caller-owned bounded slots: the caller MUST
retire existing producer/consumer fences before reuse, and drain before close.
The pass itself does not establish resource retirement or scheduling.

Three internal textures use approximately `flowWidth*flowHeight*10` bytes plus
allocation alignment and one 1x1 gate. Inputs and output slots are additional
caller-owned memory. No performance claim follows from that estimate.

## Validation and next integration boundary

CPU tests cover lifecycle, clock-domain separation, duplicate/stale frames,
budget recovery and a 300-input 30-to-120 refresh walk. Windows WARP tests execute
the actual compiled D3D12 shaders and compare forecast pixels against separately
rendered translation truth. They also cover scene cuts, NaN flow, zero confidence,
stationary frames, reset/zero lead, HUD regions, exposed borders and slot reuse.
Perfect synthetic motion validates the warp only; it is not estimated-flow
quality, physical GPU timing, RX 7900 XT validation or a capture-card test.

Before a playable mode: connect the existing GPU DIS optical-flow estimator and
its confidence to this core; add source/flow history ownership and bounded memory
admission; integrate a latest-source presentation loop that cancels stale
forecasts when a new input arrives and skips missed refresh deadlines; expose
settings and measured PC ingress age separately from estimated compensation;
validate real estimated motion, 30/60 Hz content, resets, HDR, HUD motion, and
physical capture-to-display latency on the user's hardware.

XeSS owns its generated-frame presentation. This controller explicitly refuses
provider-owned presentation; it must not be described as a zero-buffer layer on
top of XeSS. The eventual low-buffer predictive mode needs its own native
presentation path and is an alternative to XeSS interpolation.

Algorithm implementation is original project code, GPL-3.0-only, reusing the
existing descriptor/state helpers and HUD-region shader helper. No third-party
model, driver or proprietary runtime is added or changed.
