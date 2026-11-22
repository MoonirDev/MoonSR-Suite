# MoonSR Suite — Roadmap

Legend: [x] done and verified · [~] in progress · [ ] not started. "Verified" means built and run, with the evidence
in `DEVELOPMENT_LOG.md` or `BENCHMARKS.md`.

## Phase 1 — Research and architecture
- [x] Environment inspection (toolchain, GPUs)
- [x] Git repository, project layout
- [x] Subagent definitions (`.claude/agents/`), delegation verified
- [x] Technology research: AMD/Intel, Windows capture/presentation, competitors + algorithms
- [x] Technology research: NVIDIA
- [x] Architecture, module contracts, development standards
- [x] Consolidated `RESEARCH.md`

## Phase 2 — Functional foundation
- [x] GPU detection, adapter and monitor enumeration (3 GPUs, 2 monitors verified)
- [x] Windows Graphics Capture source with client-area crop (resize, shrink, close paths smoke-tested)
- [x] Borderless output window + flip-model swapchain
- [x] Scale pass infrastructure, fit / stretch / integer modes
- [x] Conventional scalers (nearest, bilinear, Catmull-Rom, Lanczos-3) and sharpening
- [x] Engine pipeline thread, stats, recovery state machine (live runs verified; device-loss recovery NOT yet exercised)
- [~] Basic UI: window / monitor / GPU / scaler selection, start-stop hotkey, live stats (builds and starts; scaling from the UI not yet exercised by a test)
- [x] Cursor handling: software cursor at the mapped position (drawing verified against GDI; on-screen hiding of the system cursor needs a human check)
- [ ] DXGI Desktop Duplication fallback
- [ ] Game profiles and persisted configuration

## Phase 3 — Upscaling engine
- [x] MoonSR Spatial v0.1 (edge-directed adaptive reconstruction) — measured; see BENCHMARKS.md
- [ ] MoonSR Spatial v0.2: cost reduction, natural-content quality, iGPU tier
- [x] Offline benchmark harness (PSNR / SSIM, GPU timing) on the real shaders
- [ ] Quality presets (Ultra Quality … Ultra Performance, Custom)
- [ ] Image-based motion estimation shared with frame generation
- [ ] Temporal stabilisation stage
- [ ] Small neural refinement (compute-shader CNN), training pipeline on own data
- [ ] LPIPS / FLIP, temporal stability metrics
- [ ] Reference comparisons (see BENCHMARKS.md for what can and cannot be measured here)

## Phase 4 — Frame generation (2X)
- [x] Frame history and timing infrastructure
- [x] Pyramidal block-matching optical flow in SM 5.0 compute, flow scale 0.25–1.0 (iteration 1, measured)
- [x] Bidirectional consistency, occlusion and confidence
- [x] Interpolation (splat to time t, warp both frames, confidence blend, fallbacks)
- [x] Scene-cut detection, static HUD protection (synthetic tests only)
- [x] Engine scheduler: evenly spaced presents, real frames never skipped (late generated frames: shown late since 2026-11-16, no longer skipped)
- [x] Automatic multiplier limiting (monitor refresh rate only; the overload back-off was removed 2026-11-16 at the owner's request)
- [~] Live 2X verified on a synthetic 30 fps source; NOT yet seen by a human, NOT tested on a real game
- [x] UI controls for frame generation (built and started; live use from the UI not exercised by a test)
- [x] Code review of `src/framegen` and the scheduler; scheduler findings fixed, generator minors open
- [ ] V-Sync-aligned pacing
- [ ] Cost reduction for entry-level GPUs (iGPU: 13.8 ms per real frame at 720p Performance)
- [ ] Small fast objects (currently fall back to a double image), semi-transparent overlay edges
- [ ] Dedicated pacing thread; present-time statistics (jitter percentiles)

## Phase 5 — Multi frame generation
- [~] 3X–6X from the original pair (no recursion): implemented in generator and scheduler; 3X verified live (24 -> 72 fps); 4X and above untested live on the capped test machine
- [ ] Adaptive multiplier (refresh rate, base FPS, GPU headroom, confidence)
- [ ] VRR-aware pacing, latency measurement end to end

## Phase 6 — Addon framework
- [ ] ABI design review and approval
- [ ] SDK headers, loader, manifest, trust controls
- [ ] Example addons
- [ ] Game integration bridge

## Phase 7 — Optimisation
- [ ] GPU timestamp profiling per pass, reduced precision, tiered paths per GPU class
- [ ] Dual-GPU path measured
- [ ] HDR path

## Phase 8 — Release
- [ ] Full benchmark suite, compatibility matrix, installer, release build

## Known technical challenges
- No motion vectors / depth / jitter on the capture path: temporal reconstruction and frame generation must
  estimate motion from images, which bounds achievable quality versus engine-integrated DLSS / FSR / XeSS.
- DLSS-class quality comparisons need the same scene rendered through DLSS; that requires a game or sample with
  DLSS on supported hardware and cannot be run on arbitrary captures.
- WGC capture rate above 60 fps depends on the Windows build (`MinUpdateInterval`, 24H2+).
- Interpolation adds at least one real frame of latency by construction.
- Cursor mapping between the small game window and the scaled output.
- Training data with clear rights for any neural model.
