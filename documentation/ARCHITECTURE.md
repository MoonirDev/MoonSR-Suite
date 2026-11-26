# MoonSR Suite — Architecture

Status: Phase 2 (functional foundation). Sections marked *planned* describe agreed direction, not existing code.

## 1. Decisions

| # | Decision | Reason |
|---|---|---|
| A1 | C++20, MSVC, CMake + Ninja, static-library modules | Low overhead, native Windows APIs, each module replaceable |
| A2 | Direct3D 11 FL 11_0 + HLSL SM 5.0 is the mandatory baseline | Reaches GTX 10 / RX 400 / Intel UHD; Lossless Scaling proves D3D11 is sufficient. D3D12 / DirectML are optional paths added later |
| A3 | Windows Graphics Capture is the primary capture method; DXGI Desktop Duplication is the planned fallback | Per-window, GPU-only, no injection. DDA is whole-monitor and adapter-bound |
| A4 | No injection into game processes by default | Anti-cheat and stability risk. Engine data comes only through the optional integration bridge |
| A5 | Presentation from our own borderless window with a flip-model swapchain | We control pacing; AMD documents that hooked overlays break frame pacing |
| A6 | GPU per pipeline stage; Auto = adapter driving the output monitor | All-Auto runs on one GPU with no copies. The Upscaling GPU is the main device (capture, upscaling, presentation); Frame generation and Neural rendering on another GPU run through `engine/remote_stages.h` with `gpu/cross_adapter.h` copies. NVIDIA neural rendering on a GPU other than the output monitor's runs in the helper process `MoonSR_NrHost.exe`, because NGX only creates features on the GPU of the process's oldest D3D device (see NR_BRIDGE.md) (2026-11-22) |
| A7 | Engine runs on its own pipeline thread; UI talks only to `engine/engine.h` | UI can be replaced or moved out of process without touching the engine |
| A8 | Shaders compiled by `fxc` at build time and embedded | No runtime compiler dependency, no startup hitch, works on every FL 11_0 driver |
| A9 | Processing in gamma-encoded SDR for now | Matches what the capture delivers; HDR/linear path is planned work |
| A10 | Custom algorithms are clean-room. Third-party code only after a recorded licence check | See `RESEARCH.md` licence table |
| A11 | Multi-frame generation computes every intermediate frame from the original pair; never recursive | Avoids accumulated error (*planned*, Phase 4-5) |
| A12 | Auto frame generation: presents on a steady grid at 1/target; each generated slot is computed at its true phase between the two real frames; V-Sync-only refresh cap. Latency is preferred over even pacing: the hold-back uses a 75th-percentile arrival-lateness margin, late generated frames are presented late (not skipped), a slot is skipped only when presentation is more than one slot behind, real frames are never skipped | Owner's choice (2026-11-22): lower latency over perfectly even pacing. Implemented as `AutoPhaseScheduler` in `src/engine/frame_pacing.h`. Fixed mode keeps the integer-multiplier scheduler. Measured figures and known limits: BENCHMARKS.md "Frame generation Fixed/Auto modes (2026-11-22)" |

## 2. Modules

```
                 +----------------------------- app (UI) ------------------------------+
                 |  uses ONLY src/engine/engine.h                                     |
                 +---------------------------------+----------------------------------+
                                                   |
                 +---------------------------------v----------------------------------+
                 | engine   pipeline thread, state machine, stats, recovery           |
                 +----+---------------+----------------+----------------+-------------+
                      |               |                |                |
               +------v-----+  +------v-----+  +-------v------+  +------v------+
               | capture    |  | upscale    |  | framegen     |  | present     |
               | WGC (DDA*) |  | ScalePass  |  | (planned)    |  | window +    |
               +------+-----+  +------+-----+  +-------+------+  | swapchain   |
                      |               |                |         +------+------+
                      +---------------+--------+-------+----------------+
                                               |
                                        +------v------+        +-----------+
                                        | gpu         |        | shaders   |
                                        | adapters,   |        | (fxc ->   |
                                        | device,     |        |  headers) |
                                        | monitors    |        +-----------+
                                        +------+------+
                                               |
                                        +------v------+
                                        | core        |  types, log, time
                                        +-------------+
```

| Module | Path | Contract header | Owner | Status |
|---|---|---|---|---|
| core | `src/core` | `types.h`, `log.h` | lead | done |
| gpu | `src/gpu` | `gpu_device.h` | gpu-rendering-engineer | Phase 2 |
| capture | `src/capture` | `capture_source.h` | gpu-rendering-engineer | Phase 2 (WGC) |
| present | `src/present` | `presenter.h` | gpu-rendering-engineer | Phase 2 |
| upscale | `src/upscale` | `scale_pass.h` | lead (infra), upscaling-engineer (algorithms) | Phase 2-3 |
| cursor | `src/cursor` | `cursor_overlay.h` | gpu-rendering-engineer | done |
| engine | `src/engine` | `engine.h` | lead | Phase 2 |
| app | `src/app` | — | ui-engineer | Phase 2 |
| framegen | `src/framegen` | `frame_generator.h` | frame-generation-engineer | iteration 1 |
| relief | `src/relief` | `relief_pass.h` | upscaling-engineer | experimental |
| aa | `src/aa` | `aa_pass.h` | upscaling-engineer (algorithms), shader-engineer (HLSL) | experimental (spatial tier T1) |
| pacing | `src/pacing` | *planned* | gpu-rendering-engineer | Phase 4-5 |
| addon | `src/addon`, `sdk/` | *planned* C ABI | addon-developer | Phase 6 |
| profiles / config | `src/config` | *planned* | ui-engineer | Phase 2-3 |
| bench | `tools/bench`, `benchmarks/` | — | benchmark-specialist | Phase 3 |

Contract headers are owned by the lead. An implementer who needs a change reports it; they do not edit the header.

## 3. Threading model

| Thread | Owns | Notes |
|---|---|---|
| UI thread | windows of the app, user input | Calls `Engine` methods only. Never blocks on GPU work |
| Pipeline thread (one per running engine) | `GpuDevice` + immediate context, `ICaptureSource`, `Presenter` + output window, `ScalePass` | COM MTA. Pumps the output window's messages. All D3D immediate-context calls happen here |
| OS capture callback threads | nothing | WGC `FrameArrived` only signals an event |

The D3D11 device has multithread protection enabled because WGC uses it internally from other threads.

## 4. Frame data flow (current)

```
game window ──DWM──> WGC frame pool (BGRA8 texture, GPU)
   AcquireFrame(): newest frame wins, older ones counted as dropped
        │  texture + contentRect (client-area crop) + capture QPC timestamp
        ▼
   ScalePass::Render(): [scaler PS] ──(optional intermediate)──> [sharpen PS] ──> swapchain back buffer
        ▼
   Presenter::Present(): flip-model, interval 0 + tearing when allowed, else interval 1
        ▼
   ReleaseFrame()   (after Present so the OS cannot recycle the texture before the draw was submitted)
```

No CPU readback, no per-frame allocation. Each captured frame is presented once, immediately. A timeout (static game
image) presents nothing.

Software cursor mode (default): the scaler renders into a kept "scene" texture instead of the back buffer; each
present copies the scene to the back buffer and draws the cursor on top (`CursorOverlay`). When the game image is
static and the cursor moved, the scene is re-composited without a new capture frame.

### Flow with frame generation (implemented, iteration 1)

`FrameGenerator::PushFrame` runs once per real frame: copy into a 2-deep history, luma pyramid, bidirectional
hierarchical block matching (8x8 blocks, zero-mean SSD + census cost), neighbour-consensus regularisation,
sub-block refinement, forward-backward consistency -> confidence, static-overlay (HUD) mask, scene-cut decision.
`Interpolate(t)` runs once per generated frame: splat both flow fields to time t, resolve, one pixel-shader pass
that warps both real frames and blends by visibility and confidence, falling back to a plain blend where
confidence is low and to the nearer real frame on a scene cut.

Engine scheduler: when real frame N arrives it is pushed, then M - 1 generated frames (t = k/M, each from the
original pair N-1, N) and finally frame N are presented one slot (frame time / M) apart. No frame is skipped for
being late and there is no frame-time cost limit (owner's decision, 2026-11-16): a slot that cannot be met is shown
as soon as it is ready and the rest of that frame's schedule shifts by the same amount. The effective multiplier is
limited only by the monitor refresh rate. When the GPU cannot keep up, frames are shown late (higher latency, and
capture drops real frames latest-wins); the status note says so.

### MoonSR temporal upscaling

The temporal scaler takes the frame generator's newest real frame and backward motion field.
It reprojects history, fills low-trust pixels with the single-frame seed, then performs area
back-projection. The history target and the final destination are written separately. With
sharpness enabled, a gamma intermediate feeds the final sharpening pass.

Generated frames use the same seed through `RenderSingle`. At sharpness 0 the seed writes
directly to the destination. With sharpening enabled it writes a temporary RGBA8 texture first.
The Spatial seed loads the 4×4 reconstruction window directly. It shares the same filter
arithmetic and 8-bit quantization between the generated-frame and history fallback shaders.
The seed variants and fused accumulation experiments are selectable through the offline
benchmark. Its history-protection trial stores reprojected lock lifetime, shading reference
and two luma samples in a ping-pong RGBA8 texture. The tiled registration variant keeps the
blur halo in compute group memory and writes directly to the reference texture. The
harness evaluates per-pass GPU timestamps, flow end-point error and edge-step evidence
outside the frame path. In experimental automatic sampling mode, a compute shader reduces
edge-step evidence from the captured input into a three-texel texture. The update shader
reads the first texel and uses area back-projection for uncertain input. The second records
the classifier's evidence and reset flag for benchmark scoring. The pooled trial keeps an
edge-count-weighted estimate in the third texel and restarts it on cuts. The adaptive-seed trial also
writes a pair of indirect draw arguments. The history fallback and `RenderSingle` submit
the existing MoonSR Spatial and covariance shaders with a zero instance count for the
inactive filter. A confirmed point model enables covariance. The padded adaptive trial
uses shared-memory back-projection and the same motion-gated point blend as the separate
passes. The footprint trial applies the sampling decision when predicting each source
sample, then uses the existing interpolated correction. The cached hull trial stores the
current colour alongside the source residual tile so neighbouring output pixels can reuse
the same anti-ringing samples. In the separate static-focus
trial the same state texture tracks frame agreement and controls the stationary seed blend.
Only benchmark scoring reads the state back.

Motion trials fit an affine displacement to adjacent refined cells and optionally
take a bounded per-pixel correction before the visibility test. HUD persistence
can keep a long-term color statistic alongside the overlay mask. These paths
remain benchmark options. The Lite trial skips the motion estimator and uses a
spatial seed and its preceding unfiltered value. At zero sharpness, the display
and next seed are written in the same draw.

### Smart parallax

Smart parallax runs at its position in Load order and on its selected GPU. Legacy three-stage orders keep their
existing relative order, with Smart parallax appended. The engine analyses the original capture once per real frame.
When Smart parallax comes before frame generation, its result feeds the generator. When it comes after, it processes
both real and generated frames. With frame generation enabled, that path blends the colour change along each safe
displacement segment. It uses the same blend on both frame types to limit changes in fine detail at phase boundaries.
When frame generation uses the same GPU and matching capture input, the generated phases can borrow its
half-resolution motion selection for height lookups in confidently tracked regions. Motion boundaries, scene
cuts and incompatible devices or inputs keep the coarse-flow path. HUD and edge checks retain their original
sampling positions.
The software cursor and frame counter are drawn after all stages. The engine creates
the pass only while enabled. Analysis stores two game-size height maps and half-size protection maps. Shading uses a
directional light and cavity darkening. Parallax adds a screen-position-driven colour lookup with an output-pixel
cap, a gradient limit, and protection checks along the sampling path.

Motion depth reuses capture-size cell flow when frame generation runs earlier on the same GPU. It uses
Parallax when compatible motion is unavailable. A robust projective fit removes the dominant plane's motion.
The remaining motion estimates relative depth, geometry boundaries, and independent motion. The pass keeps
reprojected depth through reliable turns, fades it on unreliable motion, and resets it on cuts.

When the relief GPU differs from the main device, the original capture and the stage's colour input travel over
separate size-cached cross-adapter links. A return link supplies the relief result to the rest of the pipeline. The engine
leaves the input frame usable if a transfer or relief render fails. The links are released when the stage is disabled.

For generated frames, apply samples both real-frame height maps at their motion-warped positions when flow is
available, blending by display phase. It uses the stronger protection from the two frames, keeps HUD protection in
screen coordinates, and attenuates relief where motion is unreliable or discontinuous. The generated path uses
unwarped height blending when matching motion is unavailable.

### Anti-aliasing (spatial tier T1, experimental)

Design note, contract and decisions: `documentation/research/anti_aliasing.md`. Implementation: `src/aa/aa_pass.h`,
`shaders/aa/*`.

The stage is `PipelineStage::AntiAliasing` with its own entry in `StageAdapters` (`antiAliasing`, token `aa` in
`pipelineOrder`, `StageGpuAntiAliasing` in `AppSettings::stageGpu`). Its default position is **after upscaling** (owner's
choice, 2026-11-25): the filter then works on output pixels and sees the scaler's result. The order stays user-editable;
the default order is Neural rendering, Frame generation, Upscaling, Anti-aliasing, Relief.

Load-order migration: `NormalizeOrder` keeps an existing order as it is, inserts Anti-aliasing immediately before
Relief when it is missing, and appends any other missing default stage. Unknown, duplicate or empty orders fall back to
the default, so a settings file written by 1.1.x (three stages) still loads and gains the new stage.

Anti-aliasing is a single-frame spatial pass: a simple FXAA-style filter, written clean-room from the published
description (no third-party shader code). Per pixel it reads the 3x3 luma neighbourhood and:

1. rejects flat areas and smooth gradients by a relative edge threshold (10 % of the local maximum) with an absolute
   floor (3 %) for dark scenes;
2. decides which axis the edge steps on from the larger of the N/S and E/W luma variation, and averages the two sides
   along that axis (N/S or E/W). Sampling along the gradient normal, as the first builds did, lands on the diagonal
   neighbour at 45 degrees - the same side of a staircase - so the sides look equal and nothing is blended;
3. leaves a straight, axis-aligned run alone: if the luma variation along the edge is below 10 % of the local range, the
   edge is already resolved (this is what keeps text stems and UI rules crisp);
4. skips a one-texel ridge (railing, bezel highlight, glyph stem - both sides the same background) while "Protect text
   and thin lines" is on.

The centre is blended towards the two-side average by `strength * weight`, where the weight is 0.70 / 0.85 / 1.00 for
Performance / Balanced / Quality. Alpha is written as 1.0; letterbox areas outside the game image are copied bit for bit.
The pass is skipped entirely when disabled, so the off cost is zero. Known cost of this shape: a 4 px checker keeps 78 %
of its contrast and a 2 px checker is flattened - the standard FXAA trade-off, recorded in KNOWN_ISSUES.md.

Settings live in the `[AntiAliasing]` section (mode, quality, strength, text/HUD protection) and are applied live through
`Engine::SetAaSettings`; the AA page is "Anti-aliasing". The threshold and floor are internal constants with no UI. The
diagnostic views (edge mask, blend weight, split) are developer tools reached with `moonsr_cli --aa-view`; A/B compare on
Home covers the split.

Statistics: a compute pass samples a 128x128 grid of the game image and counts pixels that are long, slanted, hard steps
- a stricter subset of the filter gates, so text and dense textures do not read as aliased - and reads the counters back a
few frames later through a staging ring (no stall).
`edgeCoverage` is a hard-edge density, not a quality measure; it is shown on Home and never changes the strength; `gpuMs`
is the median of eight timestamp pairs. See KNOWN_ISSUES.md.

When AA and Smart parallax both run, `ComposeAndPresent` applies AA first so relief shading is not blurred. A custom
order with Relief before Anti-aliasing is therefore not honoured; that is a documented limitation, not a silent
behaviour change.

### Original plan (Phase 4-5), kept for reference

```
capture ─> frame history (N real frames, timestamps)
              ├─> motion estimation at reduced "flow scale" (once per real-frame pair, bidirectional)
              ├─> confidence / occlusion / HUD mask / scene-cut
              ├─> interpolate at t = k/M for k = 1..M-1, each from the ORIGINAL pair
              └─> upscale (real and generated frames)
pacing controller ─> presentation scheduler ─> Present at evenly spaced target times
```

Frame generation by interpolation requires holding back the newest real frame, so it adds latency by design. The
pacing module will measure and report this; the UI must show captured and displayed FPS separately.

## 5. Engine state machine

`Idle → Starting → Running ⇄ Recovering → Idle | Failed`

- `Start()` blocks until the pipeline is initialised or failed, and returns the error text.
- Device removed / reset, present failure, capture error, or output window invalidated (display change):
  full teardown and re-init, up to 5 attempts (`Recovering`), then `Failed`.
- Target window closed: clean stop to `Idle` with an explanatory message in `EngineStats::error`.

## 6. Shader contract

All scaling and filter pixel shaders include `shaders/common/scale_common.hlsli` and are drawn with the fullscreen
triangle `common/fullscreen_vs.hlsl`. The viewport selects the destination rectangle; `uv` spans it 0..1.

`cbuffer ScaleConstants (b0)` — 64 bytes, mirrored by `ScaleConstants` in `src/upscale/scale_pass.cpp`:

| Field | Meaning |
|---|---|
| `srcTexSize`, `srcTexelSize` | full source texture size and reciprocal |
| `srcRectOrigin`, `srcRectSize` | source region in texels (client-area crop) |
| `dstSize`, `dstInvSize` | destination rectangle size in pixels and reciprocal |
| `sharpness` | 0..1 |
| `param1..3` | shader specific, 0 = default |

`t0` source texture, `s0` linear clamp, `s1` point clamp. Samples must stay inside the source region.
Details and the shader table: `shaders/README.md`.

Anti-aliasing shaders do not use `ScaleConstants`. `shaders/aa/aa_spatial_ps.hlsl` reads
`cbuffer AaConstants (b0)` — 64 bytes, mirrored by `AaConstants` in `src/aa/aa_pass.cpp` — with the full texture size
and texel size, the game-image rectangle, effective strength, the two edge thresholds, output scale, quality preset,
text-protection flag and debug view. `shaders/aa/aa_stats_cs.hlsl` reads `AaStatsConstants` (48 bytes) with the same
rectangle and thresholds plus the sample grid. Both are compiled by `fxc` at build time and embedded like every other
shader.

## 7. Addon framework (*planned*, Phase 6 — needs lead approval before it is frozen)

- Versioned C ABI (`sdk/include/moonsr_addon.h`): plain structs with a size field, function tables, no C++ types,
  no exceptions across the boundary.
- Categories: upscaler, frame generator, filter, capture method, presentation method, motion estimator, HUD
  detector, colour processing, game integration.
- Package = folder or archive with a manifest (id, version, author, API version, categories, GPU/API
  requirements, config schema, dependencies).
- Discovery by scanning the addons directory. Adding an addon never requires editing host code.
- Trust: untrusted by default, explicit user approval per addon, never elevated. In-process first for texture
  sharing performance; ABI designed so an addon can be moved out of process (shared NT handles) later.
- Host validates everything an addon returns and disables an addon that faults.

## 8. Game integration bridge (*planned*)

Optional shared-memory + shared-texture channel through which a cooperating game, mod or addon can publish motion
vectors, depth, jitter, exposure, reactive / HUD masks and frame timing. The universal capture path never depends
on it.

## 9. Error handling

- Every HRESULT is checked. Failures are logged with `MSR_LOG_*` and surfaced through return status plus an error
  string, never through exceptions across module boundaries.
- Capture and present report `DeviceLost` distinctly so the engine can recover.
- Log file: written by the app at startup (`InitLog`).

## Notice dismissal persistence

InfoBar keeps an in-memory list of exact dismissed messages. MainWindow loads it from the DismissedNotices settings section and wires a save callback after the close animation completes. Messages are hex-encoded UTF-16 to preserve INI-sensitive text. The Settings Restore notices button clears the list and section and relayouts the current page. Other settings sections are preserved.

## Final output screen capture

The presenter allows screen capture by default. WGC input capture uses CreateForWindow on the selected source HWND, rather than monitor capture, so capturing the final output externally does not feed that output into the input pipeline. MoonSR windows remain excluded from its own source enumeration. Game-only external capture sees the original game; display capture includes the processed output.
