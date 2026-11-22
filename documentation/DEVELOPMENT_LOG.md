# MoonSR Suite — Development Log

Newest entry first. Each entry: what was done, who did it, what was verified and how, what is next.

---

## 2026-11-21 - Session 4c: cleanup and licence for publishing on GitHub

- Owner: remove unused files and code before publishing; make the licence free to use. Decisions: MIT for MoonSR's
  code with the GPL-3.0 neural rendering bridge kept as a separate component; remove dev-only diagnostics and the
  Moon ALSR bench; untrack the Claude workflow files; publish as a fresh single commit.
- Removed: everything in `ls-addon-manager/` that the bridge does not compile (manager, installer, Upscalers addon,
  tools, docs; 33 files remain); the dead OptiScaler forwarder CMake block; `tools/tbench`, `alsr_dev.h`, the Moon
  ALSR pass profiler and the learner's tools-only API; the frame generator's `MOONSR_FG_PROFILE` profiler and
  `MOONSR_FG_DEV` switches; `MOONSR_FG_VSYNC_LEAD`; the presenter's undocumented overrides (`MOONSR_NOLAYERED`,
  `NOEXCLUDE`, `NOTOPMOST`, `BUFFERS`, `NOTEARING`, `LATENCY`); unused accessors in the UI and learner; the bridge's
  unused progress hook. Read-only sweep by an Explore agent (Sonnet), verified by the lead.
- Licence: root `LICENSE` is now MIT; the GPL-3.0 text moved to `addons/moonsr_nr_bridge/LICENSE`. New
  `THIRD_PARTY_NOTICES.md` and `README.md`. Local user paths removed from two research notes.
- Verified: full build warning-free in MoonSR code; GPU, cursor, overlay, model store, recorder, NR settings, NR
  bridge (fixture, real model), AMD fixture, Intel and NR engine smokes pass; `moonsr_bench --selftest` passes;
  `moonsr_fgbench --selftest` keeps its one pre-existing failure; live 30 fps 2X run: 58 fps displayed, 0 dropped,
  0 late.

---

## 2026-11-21 - Session 4b: neural rendering on Intel GPUs; AMD path audited

- Owner: NR must support AMD and Intel; for Intel, reference github.com/geladons/DLSS5_INTEL.
- Reference studied (shallow clone at `7e8e039`, read only): `d5c_chain.dll` runs the DLSS NR network on Vulkan
  cooperative matrices. It has a C ABI with CPU BGRA buffers and no adapter selection. The user supplies
  `dlssnr-logical.safetensors`. Its own figures: about 210 ms per frame at 960x576 on an Arc Pro B50.
- Lead: Intel backend `addons/moonsr_nr_bridge/intel_model.{h,cpp}` (readback, runtime, upload, picture pass, live
  scale and Phase 30 settings, history reset, Vulkan loader restricted to Intel drivers). Host:
  `NeuralBridge::IntelRuntimePath()` (`intel-nr\` beside MoonSR.exe), Intel accepted in `NeuralBridge::Create`, the
  engine routes Intel adapters like AMD, new `Engine::IntelNeuralRuntimeAvailable()`. Test stand-in
  `intel_fixture.cpp` and `MoonSR_IntelNrSmoke`.
- ui-engineer (Sonnet): Intel states and texts on the Neural rendering page (`src/app/main_window.cpp`, `pages.cpp`).
  Reviewed by the lead; two strings corrected (speed figure matches the source, mixed-vendor wording).
- Verified: full build (`build\lead`, `build\main`) warning-free in MoonSR code. Intel smoke passes (bit-exact
  output, settings, reset, resize, picture, driver filter: `*nv*` shows only the NVIDIA GPUs, `*amd*` only the
  Radeon). Regressions pass: NVIDIA bridge (fixture and real model), NVIDIA engine smoke, AMD bridge fixture on
  the Radeon, GPU smoke.
- Not verified: anything on a real Intel GPU or with the real DLSS5_INTEL runtime; real AMDNR (needs HIP and an
  RDNA 3+ GPU). See KNOWN_ISSUES.

---

## 2026-11-21 - Session 4: GPU priority, so neural rendering holds its speed on a busy GPU

- Owner: neural rendering performs poorly at high GPU usage; it should stay consistent until the GPU is exhausted.
- Lead (diagnosis by measurement, new `tools/gpuload`): MoonSR never raised its GPU priority. Beside a full
  competing load, NR at 720p went from 3.8 ms to 15.7 ms per frame. The bridge's D3D12 queue already followed the
  host's device priority, but with HAGS on, process class HIGH and device priority +7 changed nothing (same
  scheduling band as the game). Class REALTIME was granted without elevation and restored idle-GPU timing.
- Fix (lead): `src/engine/gpu_priority.h`, applied by the engine right after device creation and before the neural
  bridge is created: process GPU class REALTIME (fallback HIGH), device thread priority +7 (which also makes the
  bridge create a HIGH-priority D3D12 queue). Logged at start. `MOONSR_GPU_PRIORITY=normal` disables it.
- Test support: `moonsr_gpuload` (load generator and latency probe), `MoonSR_NrBridgeSmoke --priority`,
  `moonsr_cli --neural <model id>`, `MOONSR_NR_TRACE=1` per-stage bridge timings on stderr, timing line in
  `moonsr_nr_engine_smoke`.
- Verified (RTX 5060, numbers in BENCHMARKS.md): bridge under full load 4.65 ms p50 / 4.72 ms p95 (was 15.74 /
  17.90); live 120 fps source with NR under full load: 120 fps presented, 2 dropped (normal priority: 45-52 fps,
  19 dropped). Full build in `build\lead` warning-free in MoonSR code; gpu smoke, NR bridge smoke (fixture and real
  model), NR engine smoke pass. `moonsr_fgbench --selftest` has one pre-existing failure (see KNOWN_ISSUES).
- Not verified: desktop smoothness when MoonSR itself overloads the GPU; AMD; the owner's game.

---

## 2026-11-16 - Session 3: no frame-time cost limit in the scheduler

- Owner: frame generation "wasn't fast enough, so it was skipping every frame"; asked to remove the maximum
  frame-time cost for frame generation and upscaling.
- Lead: removed the late-frame skip (a generated frame more than half a slot late was dropped) and the overload
  back-off (multiplier lowered when most real frames were late). A late slot is now shown when ready and the rest of
  the schedule shifts with it. Only the refresh rate limits the multiplier. `EngineStats::generatedSkipped` became
  `generatedLate` (shown, not dropped); new status note when running late. Upscaling had no time-based limit.
- Verified: `tests\ui_fg_test.ps1` 30 fps 2X and 4X on the RTX 5060: PASS, late 0, dropped 0, 120 displayed fps at
  4X. Overload check on the Radeon 610M (correctness only), CLI 4X Lanczos 720p->1080p: stays at 4X, about
  50 generated fps shown instead of being skipped or backed off; latency rose to about 65 ms and capture dropped real
  frames (173 in 14 s). That is the expected cost of having no limit.
- In progress (agents): Moon ALSR stage 3 learning (resumed from the hand-over log), anti-aliasing for generated
  frames, study of the Lossless Scaling install and addons for motion/depth estimation, DLSS 4.5 cost ceilings.

---

## 2026-11-14 / 15 - Session 2: first real-game test, diagnostics, new UI, temporal upscaler iteration 1

- Owner tested on a real game (ATLYSS, 1280x720 on 1920x1080). Reported: frame generation not starting, upscaling
  quality far from the target.
- Frame generation failure: RivaTuner (RTSS) hooks MoonSR; a limit in its Global profile caps MoonSR's own
  presents. Reproduced, detected in-process, explained in the UI. Owner excluded MoonSR.exe; generation then worked.
- Second cause found later: the frame-rate estimate built from single arrival intervals (quantised by the
  compositor) made the refresh cap refuse 2X on a 60 fps source. Replaced by a one-second frame count.
- Diagnostic frame recorder (lossless PNG of presented frames + inputs, Ctrl+Alt+R), `moonsr_imgtool`. First look
  at live generated frames: motion correct, edge artifact on a fast bar. Owner's recording inspected: MoonSR
  Spatial output is nearly identical to the input (stair-steps preserved).
- Frame counter overlay ("game/displayed"), upscaling on/off (1:1), log kept across launches, stats logged every 5 s.
- UI rebuilt in Windows 11 Settings style (custom-drawn Direct2D, Mica, light/dark, accent) by the ui-engineer;
  screenshots reviewed by the lead; end-to-end test through the app passes.
- MoonSR Temporal iteration 1 (upscaling-engineer, three agent runs because of usage limits): builds, self-tests
  pass, measured. Verdict: NOT wired into the engine. At the owner's 1.5x aliased case it does not beat Lanczos-3
  overall, shows HUD halos and costs 4.2 ms on the RTX 5060. See BENCHMARKS.md.
- Decision pending: next upscaler direction (single-frame edge anti-aliasing reconstruction for aliased games vs
  fixing Temporal's HUD handling and cost).

---

## 2026-11-13 — Session 1d: scheduler rework after review, frame generation UI

- Code review (code-reviewer, Opus) of `d5f96d9`: accept with fixes. Generator: no memory-safety, lifetime or
  shader bounds defects; constant buffers and key packing verified. Scheduler (lead's code): 7 major findings.
- Fixed by the lead: a failed push no longer freezes the picture (frame shown directly, generation disabled after
  3 failures); no motion estimation while the effective multiplier is 1; refresh cap decided once per real frame
  from a slow frame-time estimate with hysteresis; overload judged over a 30-frame window with stepwise recovery
  probes instead of a one-way ratchet; frame-time estimate ignores hitches and accounts for dropped frames; GPU
  work submitted before the slot wait; cursor sampled after the wait; due times measured from frame arrival;
  owed real frame shown before generation is switched off.
- Found while re-testing: the short-window estimate made 2X on the 60 Hz monitor fall to 1X permanently
  (compositor jitter); and an estimate reset made the multiplier bounce back to the request. Both fixed.
- UI (ui-engineer, Sonnet): Frame generation section (mode, quality, flow scale, HUD protection), status rows for
  effective multiplier, added latency and late frames; negative window positions restored correctly.
- Verified by the lead: build warning-free; fgbench self-test passes; live runs in BENCHMARKS.md, including the
  first live 3X (24 -> 72 fps).
- Still open: V-Sync pacing, display-time verification, generator review minors, real-game test, human look.

---

## 2026-11-13 — Session 1c: frame generation, iteration 1

- Contract `src/framegen/frame_generator.h` and the engine scheduler (lead); generator, 15 shaders and
  `moonsr_fgbench` (frame-generation-engineer, Opus; interrupted once by the usage limit and resumed).
- Verified by the lead: full build warning-free; `moonsr_fgbench --selftest` 29/29; live 2X on a 30 fps source
  gives 60 displayed fps with a 16.6 ms hold-back and no skipped frames.
- Found by the live test: with presents capped near 72/s on this machine, 3X overran every frame, latency rose to
  51-55 ms and real frames were dropped. Fixed with automatic limiting (refresh-rate cap + back-off after 4
  consecutive overruns). 3X/4X requests now settle at 2X here.
- Not done: human look at live output, real game test, code review of the new code, UI controls, cost reduction
  for entry-level GPUs. See KNOWN_ISSUES.
- Next: review, UI controls, then cost reduction and small-object tracking.

---

## 2026-11-13 — Session 1b: software cursor

- Contract `src/cursor/cursor_overlay.h` (lead); implementation, shaders and `tests/cursor_smoke`
  (gpu-rendering-engineer, Opus); engine integration with a kept scene texture and `CursorMode` (lead).
- Cursor kinds handled: colour with alpha, masked colour, monochrome including invert pixels, via one dual-source
  blend pass (a two-pass invert measured up to 90/255 error under bilinear filtering; the single pass measures
  at most 2/255 against GDI `DrawIconEx`).
- Verified by the lead: full build warning-free; `moonsr_cursor_smoke` 105 checks pass; live pipeline in
  `--cursor software|captured|none` at ~60 fps with no drops; system cursor state normal after exit.
- Not verified: that the system cursor really disappears on screen while hidden (see KNOWN_ISSUES).
- Presenter `WM_QUIT` re-post fixed (review item m7).
- Next: frame generation Phase 4.

---

## 2026-11-13 — Session 1: research, architecture, functional foundation, first upscaler iteration

### State at end of session
A working application: `MoonSR.exe` (UI) and `moonsr_cli.exe` capture a window with Windows Graphics Capture,
scale it with one of five scalers (plus optional sharpening) and present it on a borderless click-through output
window. Verified live on RTX 5060, RTX A400 and an AMD Radeon iGPU, including cross-adapter operation.
No frame generation, temporal upscaling, addon framework or cursor handling yet.

### Environment
Ryzen 9 7900X, 31 GB, RTX 5060 + RTX A400 + Radeon iGPU, two monitors (1920x1080@144, 1920x1200@60),
Windows 11 build 26300, Visual Studio 2026 (MSVC 14.52), Windows SDK 26100. No Intel GPU available.

### Agent assignments and outcomes
| Task | Agent (model) | Outcome |
|---|---|---|
| Delegation smoke test | documentation-engineer (Haiku) | Passed. Custom agent types were not registered in the session that created them; fallback = general-purpose + model override + role file. They registered later in the session |
| NVIDIA research | graphics-researcher (Opus) | `research/nvidia.md` |
| AMD + Intel research | graphics-researcher (Sonnet) | `research/amd_intel.md` |
| Competitors + algorithms | graphics-researcher (Opus) | `research/capture_tools_and_algorithms.md`. "Apex Compute ANR FG" not found in any source |
| Windows capture/presentation | graphics-researcher (Sonnet) | `research/windows_capture_presentation.md` |
| GPU device, monitors, WGC capture, presenter, smoke test | gpu-rendering-engineer (Opus) | `src/gpu`, `src/capture`, `src/present`, `tests/gpu_smoke` |
| Catmull-Rom, Lanczos-3, sharpen shaders, shader README | shader-engineer (Sonnet) | `shaders/scale`, `shaders/README.md` |
| MoonSR Spatial v0.1 + reference port | upscaling-engineer (Opus) | `shaders/upscale/moonsr_spatial_ps.hlsl`, `benchmarks/reference/` |
| Win32 UI | ui-engineer (Sonnet) | `src/app` |
| Offline benchmark harness + measurements | benchmark-specialist (Sonnet) | `tools/bench`, `BENCHMARKS.md` |
| Review of the whole foundation | code-reviewer (Opus) | Accept with fixes: 3 major, 13 minor |
| Architecture, contracts, build system, scale pass, engine, CLI, test pattern, consolidation, fixes | lead (Fable) | `src/core`, `src/upscale`, `src/engine`, `tools/cli`, `tools/testpattern`, `cmake`, docs |

### Verification performed by the lead
- Full build (`tools\build.ps1`), /W4, no warnings.
- `moonsr_cli --list`: 3 hardware adapters + WARP, 2 monitors with correct refresh rates, 5 scalers.
- Live pipeline against `moonsr_testpattern` (1280x720 @ 60 fps): ~60 fps captured and presented, 0–3 frames
  dropped at start-up only, live scaler switching, no log warnings; on A400 (monitor 1), RTX 5060 (monitor 0),
  AMD iGPU and RTX 5060 processing with the A400 display (cross-adapter).
- `moonsr_bench --selftest` passes; quality and timing runs re-executed by the lead reproduce the specialist's
  numbers (timings within 0.002 ms).
- Shaders reviewed by reading (lead) and by the code reviewer (constant-buffer layout, region clamping, Gather
  component mapping).

### Measured results (details and commands in BENCHMARKS.md)
- 2x upscale, mean over 7 synthetic images, RTX 5060, no sharpening — luma PSNR / SSIM: Bilinear 25.72 / 0.770,
  Catmull-Rom 26.77 / 0.819, Lanczos-3 26.95 / 0.828, **MoonSR Spatial v0.1 27.70 / 0.834**.
- MoonSR Spatial leads on slanted edges and text, but **loses to Lanczos-3 on the "natural" image**
  (SSIM 0.912 vs 0.928 at 2x).
- Isolated pass time 1080p→4K: MoonSR Spatial 0.94 ms (RTX 5060), 6.8 ms (RTX A400), **30.3 ms (Radeon iGPU)** —
  about twice Lanczos-3 and not usable on the iGPU at that size.
- Cross-vendor output: NVIDIA cards bit-identical; Radeon within 1–2 levels.

### Bugs found and fixed
- Engine: recovery could loop forever on a per-frame fault → recover only on device loss / present error, with a
  budget of 3 recoveries per 10 s.
- Engine: frame-latency wait consumed the swapchain's initial signal, leaving one extra frame queued → wait once
  after swapchain creation.
- Engine: stop or window close during recovery reported as device failure; no exception barrier on the pipeline
  thread; no fallback when the monitor's adapter cannot create a device.
- Scale pass: source-view cache pinned old capture textures after a resize; source rectangle not validated;
  `__has_include` fallbacks could hide a missing shader.
- Capture: WGC `SystemRelativeTime` lies about one composition interval in the future → frame arrival time is used.

### Honest assessment against the project goals
- Upscaling: v0.1 is an edge-adaptive spatial filter. It beats the in-tree conventional scalers on synthetic
  edges; it is nowhere near a DLSS-class result and no DLSS, FSR or Lossless Scaling comparison has been made.
- Frame generation: not started.
- GPU compatibility: three GPUs from two vendors verified; no Intel, no pre-2020 hardware.

### Next actions (priority order)
1. Cursor handling (hide system cursor, draw mapped software cursor) — the main usability gap.
2. MoonSR Spatial v0.2: cut cost (12-tap anisotropic path, reduced precision, tier for iGPUs), fix the loss on
   natural content, isotropic fallback for small weight sums, real game captures in the test set.
3. Frame generation Phase 4: frame history + pyramidal block-matching optical flow in SM 5.0 compute, then 2X
   interpolation with confidence, then the pacing thread.
4. Remaining review items in KNOWN_ISSUES.md; PresentMon check of the presentation mode.
5. Quality presets, game profiles, DXGI Desktop Duplication fallback.
6. Addon ABI proposal for lead approval.
