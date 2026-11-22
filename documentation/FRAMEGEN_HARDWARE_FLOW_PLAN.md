# Optional hardware motion analysis for frame generation

Status: standalone driver-flow smoke test implemented and run; frame-generation integration and performance
validation remain incomplete.

`addons/moonsr_nr_bridge/bidirectional_flow.h` now owns independent forward/backward
sessions, queues both executions, invalidates temporal hints on reset, and refuses
field access until both directions have been submitted successfully. It maps the
driver direction to MoonSR's previous-to-current/current-to-previous convention.
The smoke test uses this component and verifies invalid-field access before analysis
and after reset. At 1080p it retained 0.070-pixel p95 endpoint error; a repeat measured
1.981 ms median / 2.104 ms p95 execution wall time. Input frame-slot lifetimes and
submission are still the caller's responsibility; no production engine uses it yet.

The component's `Convert` method now dispatches `flow_metrics.hlsl` into two
RGBA32F textures: frame-pixel displacement, zero-mean 6x6 RMS photometric residual,
and source luminance standard deviation. This is an intermediate refinement-like
field, not the final confidence field. It assumes full-size inputs and grid size 4;
HUD weighting and consistency remain downstream work. The 1080p smoke now reads
the converted GPU fields and verifies direction/scale and finite nonnegative
residuals with positive source variance. It retained the 0.070-pixel translation
gate. An independent CPU evaluator now recomputes bilinear reference sampling,
zero-mean RMS, and standard deviation at sparse interior and border cells. At
1080p maximum RMS differences were 0.08514 and 0.07994 luminance levels; maximum
standard-deviation differences were 0.00050 and 0.00037 levels. The gate allows
0.1 levels, accounting for GPU texture-filter precision. This checks the tested
synthetic pair, not parity across arbitrary content or HUD-weighted metrics.

Use the existing `shaders/framegen/fg_consistency_cs.hlsl` for final confidence
after exposing these two fields as a D3D11 array. Its forward/backward landing
check and normalized-residual confidence should remain shared with the software
backend; duplicating this logic in the D3D12 bridge would risk diverging behavior.

`flow_shared.h` now implements the output handoff: two shared RGBA32F textures,
D3D12-completion and D3D11-release fences, and a D3D11 two-slice array/SRV matching
the consistency pass's input layout. Publication queues GPU waits and copies;
callers must consume the array on the same immediate context before republishing,
and drain queues before destruction. The smoke test publishes three times with
separate in-flight allocators and reads both slices through D3D11, then reruns the
vector/CPU metric checks. This passed at 1080p. It verifies synthetic resource
handoff, not engine integration, capture sharing, or sustained game performance.

The smoke harness now compiles and runs the unchanged production
`fg_consistency_cs.hlsl` against that D3D11 array. The 1080p known-translation
test produced mean interior confidence 0.99924. A negative control zeroes the
reverse field while retaining the forward field and its residual; mean confidence
dropped to 0.00000. The test also checks confidence bounds and finite normalized
residual. This establishes the exercised conversion/sharing/consistency chain,
not final frame interpolation, HUD protection, scene-cut statistics, or game quality.

`flow_input.h` now adds two full-frame RGBA8 D3D11 input slots opened in D3D12.
D3D11 copies signal an input-ready fence; the D3D12 queue waits before reading.
Reusing a slot waits on its last consumed D3D12 submission, and the owner must
transition inputs to COMMON and mark the final consumed value. This helper
supports exact-size, single-mip, single-sample RGBA8 inputs only; crop/format
conversion, slot rotation in the engine, and device-loss lifetime handling remain
integration work. The smoke now uploads fixture data through a D3D11 texture and
uses this input path, retaining the same vector, residual, and confidence results
at 1080p. Its execution-only timing still excludes capture copies and conversion.

`hardware_flow_analysis.h` composes the tested components into an experimental
D3D11 pair-analysis provider. It selects the caller's adapter LUID, supports RGBA8
and BGRA8 shared input allocation, owns a four-recording allocator ring, and returns
the borrowed D3D11 intermediate array SRV. It disables unvalidated temporal hints
and drains queued work before destruction, quarantining state after drain failure.
The smoke test exercised 12 repeated RGBA8 frame pairs at 1080p with direction checks.
It still does not replace `FrameGenerator::Analyse`, has no launcher setting, and
has not established BGRA8 correctness, full-pipeline timing, or game quality.

Experimental engine connection is now available in builds configured with
`tools/build.ps1 -BuildDir build/pacing-update -Target moonsr_fgbench -ConfigureArgs '-DMOONSR_EXPERIMENTAL_HARDWARE_FLOW=ON'`.
Set `MOONSR_FG_HARDWARE_FLOW=1` for a benchmark process and use exactly 100% flow.
The frame generator replaces search/refinement/cross repair with hardware fields,
then retains production consistency, HUD, histogram scene-cut, and interpolation
passes. Initialization/analysis errors fail the requested experiment instead of
silently reporting software results. Reset discards the hardware provider.

The first integrated 640x480 `pan_5px` test produced 36.85 dB PSNR at phase 0.5,
versus 21.68 dB for blending; mean confidence was 0.988. The log confirms hardware
activation. This also exercises BGRA8 history inputs. It is a single synthetic
sequence, not proof of game quality or a cost improvement. Main builds default
to hardware support OFF, and no launcher option exists. Do not use this experiment
with adaptive learning yet: reduced-flow candidates and backend profile isolation
are not implemented. Earlier statements above describe staged component milestones;
this paragraph records the current experimental engine connection.

The adaptive controller now explicitly refuses to initialize sampling/profile loading
when the experimental hardware mode is requested. It reports an unavailable-learning
reason instead of reusing software profiles or trying unsupported reduced-flow candidates.

## Initial integrated comparison: do not promote

At 1080p on the RTX 5060, the integrated medium-preset hardware path measured
3.242 ms median PushFrame and 0.280 ms interpolation versus 2.398 and 0.258 ms
for software, in separate 100-iteration timestamp runs with matched content and
100% flow. These cross-queue measurements include synchronization dependencies;
they are not isolated OFA execution or whole-application utilization.

The complete 720p quality suite ran with hardware flow. Against the earlier
standard software CSV using the same source/configuration, hardware lost 2.112 dB
at periodic_fence phase 1/3 and 2.084 dB at pan_45px phase 1/4; real_turn differences
were approximately -0.10 to -0.18 dB. This is not an acceptable general replacement.
Results and activation logs are under `build/main/alfg-hardware-integrated`.
Keep the normal build on software. Improving input staging, selecting suitable
hardware quality/resolution, and validating temporal hints would require new
quality and full-path timing evidence before reconsidering the backend.

The provider now retains the preceding real frame's shared/packed slot when the
generator's real-frame ID advances by exactly one. Missing, duplicate, discontinuous,
or unspecified IDs rebuild both slots; pointers are never used to infer continuity.
An exception clears reusable history. The smoke exercises 12 advancing textured
pairs with an intentional ID gap and checks the output translation each time.
Integrated `pan_5px` quality remained unchanged. A subsequent 1080p run measured
3.199 ms median PushFrame versus the prior 3.242 ms; this small separate-run
difference does not establish a robust speed gain, and remains slower than software.

An experiment registered the same prepared input textures with both direction
sessions, intending to pack each input once. Its 1080p smoke process exited with
code 1 without a diagnostic before completing the checks. No assumption of
cross-session registration support is warranted from that result. The experiment
was reverted; the restored independently allocated/registered sessions passed the
complete 1080p smoke again, including moving-frame slot reuse and confidence gates.
Sharing a pack result via explicit copies into independently registered input
surfaces would be a separate experiment, not equivalent to sharing registrations.

That explicit-copy alternative is now implemented as `OpticalFlow::PrepareFrom`:
direction 0 packs an image, direction 1 copies the packed buffer into its own
registered input surface. Existing NR callers still use the original Prepare path.
The full 1080p smoke passed; the integrated pan quality CSV matched its previous
hardware baseline byte-for-byte. Alternating integrated timing medians were
3.049/3.039 ms versus 3.069/3.060 ms without reuse. This is a small saving and
still does not justify replacing software flow. The failed shared-registration
experiment remains reverted; input surfaces and handles are independent.

`MOONSR_FG_HW_HINTS=1` now optionally enables driver temporal hints only when real
frame IDs are consecutive. Missing/duplicate/discontinuous IDs disable the hints
for that pair. `MoonSR_FlowSmoke --1080p --hints` passed the moving sequence and
intentional gap tests. Explicit resets destroy the provider. Automatic camera-cut
hint invalidation remains unimplemented; hints are experimental and default OFF.

A matched 720p quality comparison with four prior real frames measured hinted
pan_45px gains of 1.25–1.75 dB and real_turn gains of 0.09–0.13 dB, but
periodic_fence losses up to 0.43 dB. Logs/CSVs are tagged `hints0` and `hints1`
under the integrated output directory. These mixed results do not justify enabling
hints generally, and no efficiency improvement has been established from them.

`MoonSR_FlowSmoke` is an opt-in CMake target. Build with
`tools/build.ps1 -BuildDir build/pacing-update -Target MoonSR_FlowSmoke`, then run
`build/pacing-update/bin/MoonSR_FlowSmoke.exe`. It uploads two synthetic 640x480
textured frames translated by (12,8), executes both directions without temporal
hints, reads the signed fixed-point flow grid, checks interior p95 vector error,
and shuts down the session. The first hardware run measured median (-11.969,-7.969)
and (12.031,8.031), with 0.070-pixel p95 endpoint error in both directions. This
confirms current-to-reference direction and 1/32-pixel scaling in this test.
The test now uses two independent direction sessions and times 100 warmed repeated
pairs with hints disabled. On the RTX 5060, medium preset at 640x480 measured
0.768 ms median / 0.834 ms p95 wall time; `--1080p` measured 1.978 / 2.066 ms,
with the same 0.070-pixel p95 endpoint error. These times include CPU submission
and completion waits but exclude input packing, flow conversion, D3D11 sharing,
and the rest of frame generation. They are not GPU timestamp measurements.

The optional `--fast` preset FAILED the 1080p translation gate: its median vector
was close to expected but p95 endpoint error was 360.347 pixels. It must not be
enabled in production based on median accuracy. The test returns failure before
timing when accuracy fails. Further content and driver investigation is required.

The test does not establish game quality, complete-pipeline performance, D3D11
sharing, concurrent direction execution, or temporal-hint correctness. It loads
no NR model or compositor. Medium-preset execution alone offers limited margin
against the current software path; integration must measure all overhead.

The remaining efficiency target is the hierarchical search and regularization in
`src/framegen/frame_generator.cpp::Analyse`. The measured RTX 5060 1080p sample
spent 1.267 ms in search and 0.636 ms in regularization. Preserve the existing
interpolation, scheduling, HUD protection, scene-cut handling, and software fallback.

## Existing reusable code

- `addons/moonsr_nr_bridge/neural_rendering/src/optical_flow.{h,cpp}` provides a
  D3D12 Optical Flow session, capability queries, registered input slots, and GPU
  fence synchronization. It loads the installed driver from System32.
- Its `gpu.{h,cpp}` and the MoonSR NR bridge already establish D3D11/D3D12 resource
  sharing. The current `NeuralBridge` interface returns enhanced images, not flow.
- `FrameGenerator::MotionField` requires forward and backward displacement,
  confidence, and normalized residual. The existing Optical Flow conversion is a
  neural-rendering guide and cannot simply be substituted for this contract.

## Smallest useful prototype

1. Add a separately testable flow-only bridge which has no dependency on the NR
   model, compositor, `nvngx_dlssnr.dll`, or user model installation. Reuse the
   checked-in Optical Flow wrapper and headers; do not download vendor files.
2. Feed the two real frames at the frame-generation stage's input, honoring load
   order. Generated images must never become analysis inputs. Keep capture times
   and pair IDs attached to every submitted slot.
3. Execute both temporal directions with independent output resources and fence
   values. Confirm vector direction, fixed-point scale, grid geometry, border
   behavior, and reset behavior using known translations before interpolation.
4. Convert both directions into the existing cell-grid format. Derive photometric
   residual and texture variance from the real pair; hardware cost is not the
   existing normalized residual. Retain forward/backward consistency and the
   downstream confidence, HUD, and scene-cut passes. Initially bypass software
   search, regularization, refinement, and cross-direction repair entirely to
   measure the hardware path's actual benefit and quality limitations.
5. Bound shared resources and in-flight work. The D3D11 consumer must wait on the
   matching GPU completion fence; no CPU wait or opportunistic use of an older
   pair's vectors. Handle device removal and resolution changes without recycling
   resources still owned by the driver.
6. Keep software as the default until measured. Unsupported drivers/devices or
   initialization failure fall back before a frame pair is scheduled. A live
   backend change resets history and discards outstanding generated schedules.

## Learning and verification gates

Backend and motion configuration must participate in saved profile identity so
software evidence never authorizes a hardware setting. Full-context held-out real
frame validation, timing gates, and per-game save/reload remain required.

Compare known translation, repeating patterns, moving foreground/occlusion, HUD,
camera rotation, scene cuts, and real game triplets at 2x and 3x phases. Measure
whole-application GPU engines, transfer/synchronization cost, GPU time, dropped
captures, output cadence, and user-visible artifacts under matched game settings.
An isolated OFA execution time does not prove a cheaper complete pipeline. The
NVIDIA implementation remains optional and makes no claim of official support.
