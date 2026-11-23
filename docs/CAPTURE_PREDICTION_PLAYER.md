# GPU capture prediction player

This unofficial Windows x64 capture-focused build integrates prediction into
Veyra 2.1.9. It is an experimental alternative to ordinary interpolation for
PS5 capture-card viewing, including 30 Hz content displayed at 120 Hz.

## Enable and tune

1. Use a fresh copy of the original AMD 2.1.9 portable folder. Extract the
   compiled update into it, replacing the executable, QML and shaders.
   Alternatively extract the full portable build into its own directory.
2. Open the Frame Generation tab and enable **GPU capture prediction**.
3. Reopen capture so its device allocator switches to the minimum mode.
4. Start with **8 ms compensation, 120 Hz refresh, 33 ms maximum horizon,
   and HUD protection enabled**. Match refresh to your monitor. Increase
   compensation gradually only if the motion remains acceptable.

The same controls appear in the pinned frame-generation node editor. Values
persist across restarts. Enabling this mode disables interpolation nodes;
enabling ordinary frame generation disables prediction. It does not stack on
XeSS 4X. Presentation uses tearing, display capacity one, and the mode's own
refresh setting. Conflicting presentation controls are disabled while enabled.
Recordings and replay retain their original source timeline, without speculative
preview refreshes. The original application remains available separately.

## What the mode does

GPU ingress converts the newest image to scene-linear RGBA16F. Existing GPU DIS
estimates bidirectional motion at up to 640 pixels on the longest axis. The
pipeline validates photometric and forward/backward consistency, estimates
bounded acceleration from consistent history, and performs an inverse motion
warp. Scene changes, newly exposed borders, invalid motion, low confidence and
protected HUD areas fall back to the latest real image. Default HUD protection
covers the top strip, bottom corners and central reticle; it is a fixed mask,
not semantic HUD recognition.

Forecasts use only images already received. Between arrivals, the native
presenter can update the predicted image at the selected rate. A new source
always takes priority. Missed deadlines are skipped; forecasts are submitted
only when the GPU presentation queue is empty and display capacity is available.
No future-frame wait or additional software frame queue is added. Two retained
GPU image surfaces are motion history, not a delayed playback buffer. The capture
driver still needs its minimum mandatory allocation; hardware and driver buffers
cannot be removed by this application.

Total horizon is capped by the configured maximum and one observed source
interval. Observed cadence breaks, epoch/revision changes and graph recreation
invalidate history. Stale input stops extrapolation. Completed GPU timestamps
limit optional work to the smaller of 2 ms or 30% of a refresh period. An
overshoot bypasses ingress, flow and prediction for eight source inputs before
reseeding history. Memory admission is bounded by a conservative estimate and
available local video-memory budget. Unsupported output geometry/format or
missing display-capacity control retains original presentation with a status.

The status distinguishes requested prediction horizon, observed PC ingress age,
and measured GPU cost. Neither the compensation setting nor source PTS measures
capture-card latency. GPU measurements are completed timestamps, not estimates
of input-to-photon savings. No normal-path pixel readback or per-pass CPU fence
wait is added.

## Limits

Prediction advances already visible motion; it cannot recover an unobserved
controller action, unseen geometry, PS5 rendering time, or arbitrary card delay.
Rapid reversals, camera cuts, occlusion and particle effects can reveal artifacts.
One interval is an upper bound, not a guarantee of reliable compensation. More
sophistication does not remove those information limits.

Supported ingress surfaces are SDR RGBA8/BGRA8, HDR10 RGB10A2, and linear HDR
RGBA16F up to 3840x2160. HDR conversion is implemented but physical HDR capture
and display have not been tested in this workspace. RX 7900 XT performance,
sustained 4K cost, gameplay quality, and optical input-to-display latency require
measurement on the user's hardware. No measured milliseconds saved are claimed.

Optional Remote Play, Moonlight, Xbox, external DeckLink SDK, static libass and
proprietary AMD optical-flow SDK are not compiled into this capture-focused
executable. Bundled original runtime DLLs are preserved byte for byte. GPU DIS
uses the existing open implementation; no new proprietary model is added.

## Build and source

The fork uses three reviewed source deltas: latency-source.patch,
capture-prediction-cmake.patch, then capture-prediction-player.patch. The
Windows player workflow applies them before configuring. The compiled update
also includes source-snapshot.zip containing the already-patched build source;
do not apply the patches again to that snapshot. Build settings and executable
SHA-256 are in BUILD_OPTIONS.json and BUILD_RESULT.json.

Build with MSVC 2022, Qt 6.8.3, Ninja and the pinned dependency preparation script.
Use at most two compiler processes. Validation targets cover controller timing,
standalone prediction shaders, actual estimated-motion GPU integration, real
preference persistence, and both QML control placements. A packaged GUI startup
check uses a private profile. See the workflow run for actual results.

## Validation record

The first full-player compile caught an ownership guard accidentally placed in
removeEffect; it was moved to accepted frame-generation enablement. The next
build compiled the complete executable and test targets, and the controller
(2434 checks) and standalone WARP shader tests (9303 checks) passed. The newly
integrated session test then caught a device failure: ingress used a BGRA typed
UAV. It now writes RGBA8, while the optical-flow wrapper accepts both RGBA and
BGRA shader-resource inputs. The GPU harness reports validation messages before
throwing a device-health failure. Subsequent workflow results are the acceptance
record; earlier failed attempts are retained as evidence.
