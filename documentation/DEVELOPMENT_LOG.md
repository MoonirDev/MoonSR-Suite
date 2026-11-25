# MoonSR Suite — Development Log

Newest entry first. Each entry: what was done, who did it, what was verified and how, what is next.

---

## 2026-11-25 - Persistent notice dismissal (1.1.3)

- Codex saved exact dismissed messages in settings.ini and added a one-shot Settings switch to restore them. Changed messages still appear; the existing closing animation is retained.
- Isolated full build, upscaling and frame-generation self-tests, and theme self-test passed. A focused C++20 test verified persistence across processes, quoted/Unicode messages, reset, and preservation of other settings.


## 2026-11-25 - Smart Parallax stability (1.1.2)

- Codex packaged the owner-approved halo playtest, including grain-resistant HUD learning, selected motion
  for compatible generated frames, edge protection and the faster height pass. The isolated full build,
  upscaling, frame generation, theme, Shading, Parallax and Motion Depth checks passed on the RTX 5060.

## 2026-11-25 - Reuse frame-generation motion for Motion Depth

- The revised objective limits relief to motion already analysed by frame generation. Codex removed the dedicated
  estimator and updated the mode and Load order descriptions. Motion Depth uses Parallax when compatible motion
  is unavailable. The isolated halo playtest was rebuilt.
- Windows capture on the RTX 5060 verified Parallax fallback with generation off or later in Load order,
  and active Motion Depth when compatible frame generation ran first.
- On the rebuilt halo playtest, the fallback also stayed active with frame generation on the RTX A400, with
  Smart Parallax on the RTX A400, and with 640×360 capture upscaled to 1920×1080 before frame generation.
- An isolated Release packaging rehearsal with `tools/make_installer.ps1` produced a 2,949,736-byte installer
  (2.81 MiB). All four shipped binaries reported version 1.1.1. Inno Setup packaged the four binaries,
  README, licences and notices from the existing manifest. The rehearsal was kept under the ignored build
  directory and was never run.
- The full isolated Release build also passed the upscaling, frame generation, theme, strength-zero
  and Motion Depth self-tests. GPU tests used the RTX 5060.
- The Release and playtest builds produced byte-identical exports for all 24 paired images from
  the saved Helldivers 2 and ATLYSS inputs at 50% and 100% Parallax on the RTX 5060.

## 2026-11-24 - Smart parallax flicker playtest

- The owner saw Parallax flicker at 4× in ATLYSS and pulsing or flicker at 2× and 4× in Helldivers 2. They also
  reported shimmer on bright surfaces in Motion depth.
- Codex made generated-frame relief strength approach the newest real frame gradually. The strict HUD mask and
  path checks still apply. When matched motion supplies a scene-cut decision, relief uses it. The unwarped luma
  comparison remains available for captures without that decision.
- Motion depth now tapers the extra direction cue, depth boost, and displacement on bright surfaces. The local
  replay isolated relief from the frame generator. The first live retest showed less pulsing, with shimmer
  remaining after frame generation. The owner saw better stability with Smart parallax before frame generation.
- After frame generation, Parallax now transfers the average colour change along the checked sampling path and
  preserves finer detail from the generated image. It uses the same treatment on real and generated frames.
  The owner reported less shimmer in the follow-up playtest and could still see it.
- HUD learning now requires temporal changes with neighbouring support. The 90-frame grain test stayed active on
  all ten material inputs while the opaque HUD regression remained pixel-exact. The revised Shading pass stayed
  within its 1080p timing budget in the measured runs.
- The next candidate also smooths the height used for displacement after frame generation. The flow-aligned
  replay showed a small improvement in both Helldivers 2 clips. Lighting and the before-generation path keep
  their existing height samples.
- Offline replay compared the two real-frame height maps with height recomputed from the generated images.
  Both the full analysis and band-pass trials increased aligned error and added GPU work. The playtest retains
  the two real-frame height maps. Shimmer after frame generation remains open.
- A displacement taper and interpolation of coarse motion had little effect in the saved clips. Sharing frame
  generation's selected motion for height sampling reduced error at every tested phase against the generator's
  own smooth-region warp. The coarser reference favoured the earlier build. The next playtest borrows this motion
  for generated frames when the capture and GPU match and retains coarse flow around motion boundaries.
  Synthetic checks verified that coherent selection changes the height sample, low confidence falls back, and
  real frames stay pixel-exact on the RTX 5060, RTX A400 and Radeon iGPU.
- The RTX 5060 Shading timing runs showed two clusters after idle gaps. A high-resolution timer kept the
  pauses near 2 ms, and the slower cluster persisted. Per-pass timestamps located the largest time increases
  in luminance, gating and height. The sRGB and cache trials were kept out of the app.
- The height shader now calculates four adjacent pixels per thread. Two loaded Shading runs measured
  0.2399 and 0.2398 ms at 1080p on the RTX 5060. All 312 paired benchmark PNGs matched. The isolated full
  build and the upscaling, frame-generation, theme, Shading, Parallax, strength-zero and Motion depth checks
  passed. The slowest high-resolution idle-gap run still exceeded the target.

## 2026-11-24 - Dismissible notices (1.1.1)

- Codex restored the notice and warning close buttons in the standard and Retro layouts. With visual effects on,
  the bar shrinks and fades before the page closes the gap. With effects off, it closes immediately.
- A dismissed message stays hidden for the current run. A different message can appear immediately, including
  when it replaces a notice mid-animation.
- The isolated full build, upscaling and frame-generation self-tests, and theme self-test passed. A focused test
  exercised the close hit area, animated collapse, repeated and replacement messages, and immediate closing in Retro.

## 2026-11-23 - Smart parallax (1.1.0)

- Hardened Shading against shadow noise while retaining the strict opaque HUD comparison and the flat-scene
  auto-pause. The owner reported slightly deeper textures in ATLYSS, more subtle depth in Helldivers 2, and no
  return of the false pause in Helldivers 2.
- Added Parallax, with position-based displacement, a gradient limit and HUD and edge checks along its sampling
  path. Tuned the displacement after the owner's A/B tests. The owner confirmed that the tuned effect holds depth
  better in motion in Helldivers 2.
- Added experimental Motion depth based on a projective plane fit to existing frame-generator motion. It uses
  residual flow for coarse relative depth and suppression around independent motion and depth discontinuities.
  It falls back to Parallax when matching flow is unavailable. No motion estimator is started for relief.
- Generated frames now blend the two real-frame height maps, warp with matching motion when available, and keep the
  more protective mask at each position. The synthetic motion tests passed on the RTX 5060, RTX A400 and Radeon
  iGPU. Live Motion depth and generated-frame checks at 2× and 4× are pending.
- The owner requested that Smart parallax move into Load order, gain a GPU selector and estimate motion when frame
  generation cannot provide a compatible field. Legacy three-stage orders retain their sequence. Live capture of
  the test pattern verified Motion depth with generation off, on either side of generation, before temporal
  upscaling, and with relief on the A400 and Upscaling on the RTX 5060. Both cross-adapter paths passed.
- Shading met the 1080p budget in the loaded test. The idle-gap measurement remained above target.
- The neural rendering forwarder now embeds the version from the same generated header as the other shipped binaries.

## 2026-11-22 - Session 8: version 1.0.0, installer, Smart parallax groundwork

- **Version 1.0.0.** Single source: root `CMakeLists.txt` `project(MoonSR VERSION ...)` -> generated `core/version.h`
  -> .exe version resources (MoonSR.exe, MoonSR_NrHost.exe, MoonSR_NrBridge.dll), the Settings page and the installer.
- **Static C runtime** (`CMAKE_MSVC_RUNTIME_LIBRARY` MultiThreaded): no Visual C++ Redistributable needed; verified
  by scanning the shipped binaries for VCRUNTIME/MSVCP imports.
- **Installer** (owner: small, offline, update in place, keep settings, uninstaller): Inno Setup 6, `installer/MoonSR.iss`,
  `tools/make_installer.ps1` -> `dist/MoonSR-Setup-<version>.exe`. Per-user (`%LOCALAPPDATA%\Programs\MoonSR`, no UAC),
  LZMA2 ultra solid, fixed AppId, AppMutex closes/blocks a running MoonSR, downgrade warning, uninstaller asks before
  deleting `%LOCALAPPDATA%\MoonSR` (default keep). Ships only MoonSR.exe, MoonSR_NrHost.exe, MoonSR_NrBridge.dll,
  NeuralRendering_nvngx.dll, README, licences and notices. Size 2.73 MiB. Silent tests: install, in-place update to a
  test version (one entry, same folder), downgrade refused, uninstall clean; settings.ini hash and timestamp unchanged
  throughout. Interactive wizard and the uninstall question not clicked through. Unsigned (SmartScreen warning).
- **Smart parallax groundwork.** Research: `documentation/research/screen_space_relief.md` (screen-space relief: guided
  band-pass height, relief shading, position-dependent parallax capped at 1-3 px, plane+parallax depth only when the
  frame generator already runs; neural depth deferred). Lead decisions: two-rate stage at the end of the pipeline,
  outside the load order; Upscaling GPU; never start the motion estimator for relief; off by default; name "Smart
  parallax" (experimental); go/no-go after M2 on the owner's real-game A/B. Contract `src/relief/relief_pass.h`;
  pass-through implementation (bit-exact: 0 of 2,073,600 pixels differ; Apply 0.017 ms RTX 5060, 0.19 ms RTX A400 at
  1920x1080/1920x1200), engine wiring (Analyse per real frame, Apply per displayed frame before overlays), UI tab,
  `[Relief]` settings, CLI `--relief`, `--relief-strength`, `--relief-selftest`. Zero cost when off rests on the code
  path (nothing created); a pacing A/B could not be measured because other GPU work was running on the RTX 5060.

## 2026-11-22 - Session 7: frame generation Fixed/Auto modes, Settings page and themes

Owner requests, in order. Nothing committed yet (uncommitted working tree).

- **Frame generation modes.** Fixed multiplier now 2x to 20x (was 2 to 6), with a slider in the UI. New Auto mode: the
  user types a target frame rate (30 to 1000). The engine presents on a steady grid at 1/target and generates each slot
  at its true phase between the two real frames (`AutoPhaseScheduler`, `src/engine/frame_pacing.h`), so generated
  frames per real frame vary and can average a fraction. With V-Sync off nothing is capped to the display refresh
  rate; with V-Sync on, Fixed and Auto are capped to the refresh rate with a plain-language note, and live V-Sync
  toggles re-evaluate the cap. Auto favours lower latency over perfectly even pacing (owner's choice; see
  ARCHITECTURE.md A12). An Auto GPU-limit controller lowers the target when more than 10% of generated slots are
  skipped in two consecutive windows, and raises it again after 10 quiet windows. Only exercised in the fgbench
  simulation; it did not trigger on the RTX 5060.
- **UI.** Frame generation page: Mode selector (Fixed / Auto), 2x to 20x slider, target-fps box (new `ui::NumberBox`
  control). Settings keys: `[FrameGen] Enabled, Mode, TargetFps, Multiplier, LastMultiplier` (2..20). Older settings
  files load.
- **CLI.** `--fg 1..20`, `--fg-auto <fps>` (overrides `--fg`), `--vsync-switch-at <s>`, and a Summary line.
- **Settings page.** Diagnostics page renamed Settings. New Themes section with live-preview cards: Windows 11 Light,
  Windows 11 Dark (default), Retro (green-on-black console look, Cascadia Mono / Consolas, square boxes, bracketed
  controls). Custom `.moontheme` files can be imported (format: `documentation/THEMES.md`; example:
  `documentation/themes/example.moontheme`), imported themes can be removed, and the current theme can be exported.
  Saved as `[App] Theme`. The app no longer follows the Windows light/dark setting.
- **Verification (lead).** RTX 5060, 1920x1080 at 144 Hz, 60 fps `moonsr_testpattern --motion-pattern 1`, 8 s runs,
  averages of seconds 3 to 8, V-Sync off unless stated. Table and setup in BENCHMARKS.md ("Frame generation
  Fixed/Auto modes (2026-11-22)"). `--fg 20` gave 1201.1 fps at ratio 19.93 with 0 skipped; `--fg-auto 120` gave
  117.4 fps; `--fg-auto 300 --vsync` was capped to 144. Latency (frame-generation agent, same setup): Fixed 2x 8.4 ms,
  Auto 120 about 10.7 ms, Auto 300 16.0 ms. Displayed FPS is not responsiveness.
- **Verified by the lead.** The runs in BENCHMARKS.md. Build result for this session: not recorded in this log (not
  verified here). Theme logic covered by a 36-check self-test
  (`--test-theme-selftest`). Not verified: saving the theme choice to `settings.ini` and the import, export and
  remove dialogs were not clicked through live.
- **Known issues** added to KNOWN_ISSUES.md ("Frame generation Fixed/Auto modes (2026-11-22)" and "Settings page and
  themes (2026-11-22)").
- Next: live test of the Auto GPU-limit path; Auto latency at fractional ratios (14-17 ms); wording of the
  "irregular frame times" note at 300 fps; Retro label truncation; theme save and import/export/remove by hand.

---

## 2026-11-21/22 - Session 6: NVIDIA NR helper process, learning removed, MoonSR rename, non-topmost output, tray

Owner decisions: the quality floor for MoonSR upscaling is XeSS 2 / FSR 4-class at the current cost; online learning
is scrapped; "Moon ALSR" is renamed MoonSR in the product. Hardware: RTX 5060 + RTX A400 + Radeon 610M, driver 621.14,
Windows 11 build 26300.

- **NVIDIA neural rendering on any GPU.** Root cause of the 0xBAD00002 failure: NGX only creates features on the GPU
  of the process's oldest live D3D11/D3D12 device. In `MoonSR.exe` the Direct2D UI device is the oldest. WGC,
  swapchain, RTSS, GPU priority and cross-adapter links were ruled out. Fix: helper process `MoonSR_NrHost.exe`
  (shared NT-handle textures, two shared D3D11 fences, named pipe protocol v2, kill-on-close job, 30 s / 2 s
  timeouts), used when NVIDIA NR runs off the monitor's GPU, and as an automatic fallback when in-process creation is
  refused. New smoke option `--first-device`. CLI results (6 s, 1280x720 test pattern): `--gpu-nr A400` 87 frames and
  0 errors, `--adapter A400` 91, `--gpu-up A400 --gpu-nr 5060` 339, all-5060 unchanged. Helper cost per frame not
  measured; not tested in the GUI. Details: NR_BRIDGE.md, ARCHITECTURE.md A6, KNOWN_ISSUES.md.
- **Online learning removed.** Moon ALSR learning (`alsr_learn`, learned filters, per-game profiles) and frame
  generation adaptive learning (controller, sampler, policy, profile) are deleted with their UI, settings keys
  (ignored in old settings files), CLI flags (`--nolearn`, `--fg-learn`, `--fg-reset-at`, `--fg-disable-at`; timing
  diagnostics now under `--fg-diag`), `moonsr_bench --alsr-learning` and the fgbench adaptive tests. Testpattern
  `--learning-pattern` is now `--motion-pattern`. Default output is bit-identical before and after: 36/36 temporal
  harness rows had identical hashes. The earlier baseline had measured learning at +0.001 to +0.012 dB at 1.5x and no
  effect at 2x (BENCHMARKS.md).
- **Rename.** The scaler list shows "MoonSR (experimental)"; "MoonSR Spatial" is unchanged. CLI `--scaler moonsr`,
  alias `alsr` kept. Saved settings naming "Moon ALSR..." map to MoonSR. Internal identifiers (`ScalerKind::MoonALSR`,
  `alsr_*` files) are unchanged. BENCHMARKS.md has a naming note; recorded numbers are unchanged.
- **Output window not topmost.** Normal borderless click-through, no-activate window over the output monitor that
  keeps itself directly above the captured game window (WinEvent hooks + 100 ms poll; only MoonSR's own window is
  moved). Other windows can be dragged over it and alt-tab works. Clicking into the game restacks within a few ms
  (0.7-3 ms after foreground change in one test, 22 ms end to end in another). Hidden while the game is minimised or
  cloaked. Windows hides the taskbar while the game is foreground (observed shell heuristic). While another window
  overlaps, DWM composes (no independent flip). `PresentOptions::topmost=true` keeps the old behaviour and is not
  exposed in the UI. The launcher no longer toggles itself topmost. The software cursor only draws when the cursor is
  over the game window. Pacing unchanged: 120 fps FG 2x median 8.04-8.15 ms before and after, 0 dropped. Not tested:
  elevated games, a real game.
- **Close to system tray.** New setting "Close to system tray" (default off), Diagnostics page, "Application" section,
  saved as `[App] CloseToTray`. When on, closing hides the window; scaling continues; the icon exists only while
  hidden; click restores; right-click menu: Open MoonSR, Start scaling (countdown), Stop scaling, Exit. Launching
  MoonSR again restores the hidden window. A scripted test passed; a real mouse click on the icon and an Explorer
  restart were not tested.
- **Research.** `research/alsr_v2.md` (MoonSR v2 audit and plan; the capture-only path has no jitter and cannot reach
  parity with XeSS 2 / FSR 4 on static content) and `research/fg_quality.md` (frame generation base-quality audit and
  plan). Both are indexed in RESEARCH.md; their milestones are listed in ROADMAP.md as the next work.
- **Temporal harness.** `moonsr_bench --temporal` with its baseline is recorded in BENCHMARKS.md ("Moon ALSR baseline
  (temporal harness), 2026-11-21"); it was used for the learning-removal bit-identity check.
- Next: MoonSR v2 M0 and frame generation M0 (harnesses and baselines) per the roadmap; GUI test of the NR helper
  path; helper log pruning.

---

## 2026-11-22 - Session 5: GPU per pipeline stage; Output page removed

- Owner: remove the Output page, move Monitor to Home, remove the Processing GPU picker, and choose a GPU for each
  stage next to it in Load order.
- Lead: contracts `EngineConfig::stageAdapters` / `EngineStats::stageGpuText` (engine.h) and `CrossAdapterLink`
  (src/gpu/cross_adapter.h). Engine: the Upscaling GPU is the main device (capture, upscaling, presentation);
  Frame generation and Neural rendering on another GPU run through wrappers (`src/engine/remote_stages.*`) that
  implement the stage interfaces and copy frames across, so the frame loop is unchanged. Real frames stay local; one
  copy up per real frame and one back per generated frame. Moon ALSR uses a local motion estimator when frame
  generation is remote. CLI: `--gpu-nr`, `--gpu-fg`, `--gpu-up`; prints the GPU per stage.
- gpu-rendering-engineer (Opus): `CrossAdapterLink`. D3D12 cross-adapter heap (system-memory buffer, striped,
  shared fences, ring of 2), CPU-staging fallback, `MOONSR_XADAPTER_FORCE_CPU`, `MOONSR_XADAPTER_STRIPES`;
  `moonsr_cross_adapter_smoke` (6 pairs bit-exact, debug layers clean). Reviewed by the lead; benchmark row reproduced.
- ui-engineer (Sonnet): Output page removed, Monitor on Home, GPU picker per stage inside the order list (follows
  reordering), locked while scaling, per-stage settings with migration of the old adapter setting, "GPU per stage"
  in Home details. Lead: "Auto" label shortened, help text extended, NVIDIA hint added. Screenshots reviewed.
- Found while testing: NVIDIA neural rendering fails on a GPU other than the presenting one (NGX FAIL_PlatformError).
  Seven eliminations (feature level, priority, input texture, second D3D12 device, link before/after, shared D3D11
  device, warm-up before links) did not find the cause; the engine now refuses that combination with a clear message.
- Verified: full build warning-free in MoonSR code; cross-adapter smoke; live runs in BENCHMARKS.md (all Auto, FG on
  A400, main on A400, FG on A400 with ALSR, FG on Radeon); `tests\ui_fg_test.ps1` through the app PASS; GPU, NR bridge,
  Intel and NR engine smokes pass; fgbench keeps its one pre-existing failure.

---

## 2026-11-21 - Session 4d: ls-addon-manager folder folded into the bridge

- Owner: remove the `ls-addon-manager` folder. Its remaining files were the bridge's build inputs, so they moved into
  the bridge: `addons/moonsr_nr_bridge/neural_rendering/` (the NeuralRendering subset, GPL-3.0-only, with its
  LICENSE, NOTICE and licences) and `addons/moonsr_nr_bridge/third_party/eam/real_frame.h` (MIT, with its LICENSE).
  Bridge CMake paths, LICENSE, README, THIRD_PARTY_NOTICES, NR_BRIDGE and the hardware-flow plan updated.

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
