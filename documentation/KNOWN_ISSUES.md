# MoonSR Suite — Known Issues

Updated: 2026-11-13

## Frame generation (iteration 1)
- Nobody has looked at live generated frames yet: the output window is excluded from capture, so live image
  quality is unverified. Quality numbers come from synthetic sequences with ground truth only. No real game tested.
- Interpolation holds every real frame back by (M - 1) / M of a frame time (16.6 ms at 30 fps, 2X). This is added
  latency by design and is included in the reported latency.
- Small fast objects (smaller than ~2 coarse blocks, moving more than their own size) are not tracked: they
  appear as two half-transparent copies, like a plain blend.
- Sub-pixel motion: at t <= 1/3 the result is worse than simply repeating the nearest frame (resampling blur).
- Semi-transparent overlays and anti-aliased overlay edges tear or ghost. Animated HUD elements are warped.
- Zoom / rotation: edge misregistration (per-cell translation model).
- flowScale 0.25 is 4-6 dB worse than 0.5 on most synthetic sequences.
- Cost is too high for entry-level GPUs: Radeon iGPU 13.8 ms per real frame + 2.6 ms per generated frame at
  720p Performance / flowScale 0.5; RTX A400 3.0 ms + 0.6 ms.
- Multipliers above 3X are untested live: the development machine caps presents near 72/s, so 4X of a 30 fps
  source backs off to 2X (as designed). 3X ran live on a 24 fps source (72 displayed fps, hold-back 27-28 ms).
- V-Sync together with frame generation paces badly: on the 60 Hz monitor 2X of a 30 fps source showed only
  about 21 generated frames/s with ~5 late skips per second. The scheduler is not aligned to vblank yet.
  Use V-Sync off with frame generation for now.
- Frames whose arrival time does not fit the frame-time estimate (hitch, compositor jitter) are shown immediately
  without generated frames. On the test pattern this shows as 26-30 generated frames/s instead of a steady 30.
- Pacing is by CPU submit time. GPU work for a generated frame is submitted before its slot, but the first
  generated frame can still queue behind the motion analysis on slow GPUs, so display intervals are not verified
  to be even. Needs present-time statistics (PresentMon) and a GPU-completion estimate.
- Pacing runs on the pipeline thread (timer + up to 1.5 ms spin per present); no dedicated pacing thread.
- Scene cuts: the picture is correct (the shader outputs the nearer real frame), but the scheduler cannot see
  the cut in time, so it still holds the new frame back and shows copies. Duplicate frames from the game (e.g.
  30 fps content presented at 60 Hz) are not detected.
- Generator review items still open (owner: frame-generation-engineer): temporal candidate survives a scene cut;
  motion-statistic overflow at 4K / flowScale 1 with very large motion (diagnostic only); development profiler
  query misuse; fgbench times only one sequence and its quality sequences are the ones the parameters were tuned on.

## Unimplemented
- Adaptive multi-frame generation beyond the automatic limiting; dedicated pacing controller.
- Temporal upscaling, neural upscaling, quality presets.
- Addon framework and game integration bridge.
- DXGI Desktop Duplication fallback capture.
- Game profiles, persisted settings.
- HDR capture and output (pipeline is SDR BGRA8, gamma-space processing).
- GPU-time measurement per pass (only CPU-side pipeline time and capture-to-present latency are tracked).

## Functional limitations
- Cursor (software mode, default): MoonSR draws the cursor at the mapped position and asks Windows to hide the
  system cursor over the game (Magnification API). The drawing is verified pixel-for-pixel against GDI; **whether
  the system cursor is actually hidden on screen is NOT verified** (`GetCursorInfo` still reports it as showing,
  and the output cannot be screenshotted). Needs a look by a human.
- The software cursor only updates when MoonSR presents: with a game frame, or within about 4 ms polling when
  the game image is static.
- Animated cursors show their first frame only. Cursor size at high DPI / non-default cursor size is untested.
- The cursor is not confined to the game window (no `ClipCursor`); moving it outside shows the system cursor at
  its real position and clicks go to whatever is underneath the click-through output.
- Exclusive-fullscreen games cannot be captured through WGC; use windowed or borderless.
- When the game image does not change, nothing is presented (the last frame stays on screen).
- Recovery from device loss is a full pipeline rebuild; the output blanks briefly.

## Unverified
- Behaviour on Windows 10 builds older than 2004 (capture exclusion and cursor capture toggles are newer APIs).
- Whether the output window achieves independent flip on each GPU vendor.
- End-to-end latency. The reported figure is frame arrival (WGC `FrameArrived`) to `Present()` return: MoonSR's
  own processing delay. It excludes game render, DWM composition/capture delay and display scan-out. WGC's
  `SystemRelativeTime` was observed about one composition interval in the future on build 26300 and is not used
  as a capture time. True end-to-end latency needs external measurement.

## Research gaps
- "Apex Compute ANR FG", named in the project brief as a quality benchmark, could not be found in any public
  source (see `research/capture_tools_and_algorithms.md` section 1.3). It is not used as a target until a
  reference is supplied.
- AMD FidelityFX SDK licence is per-file; nothing from it may be reused before a file-level check.

## Open items from the Phase 2 code review (2026-11-13)
Fixed in the same session: unbounded recovery loop, frame-latency wait off by one, stale source-view cache,
stop-during-recovery reported as failure, missing exception barrier on the pipeline thread, no adapter fallback
for display-only adapters, unvalidated source rectangle, silent shader fallbacks.

Still open:
- Settings: negative window coordinates are not restored (`GetPrivateProfileInt` returns 0). Owner: ui-engineer.
- Output monitor is held as an `HMONITOR`; a display change during recovery may invalidate it. Should be resolved
  from the device name. Owner: lead + ui-engineer.
- Capture frame is released to WGC right after `Present`; under heavy GPU load the OS could in theory overwrite
  it before our draw executed. Not observed. Needs a frame-counter stress test.
- `WS_EX_LAYERED` on the output window may prevent independent flip. Needs a PresentMon check.
- Client-area crop is untested for DPI-unaware target windows on scaled monitors, and for maximised windows.
- MoonSR Spatial: anisotropic weight sum is not proven positive for all inputs (output is bounded by the soft
  clamp, so no NaN, but a pixel could snap to a bound). Add an isotropic fallback for small sums in v0.2.
- Dropped-frame accounting counts frames discarded during a capture-pool resize as game frames dropped.

## Environment notes
- On the development machine every present path measured about 72 presents/s regardless of settings while RTSS
  / MSI Afterburner were running; a global frame limiter is the likely cause (unverified). Uncapped presentation
  rate and WGC delivery above 72 fps are therefore unmeasured.

## External frame limiters (found 2026-11-14 from a user report)
- RivaTuner Statistics Server (RTSS / MSI Afterburner) injects into every Direct3D process, including MoonSR.
  A frame limit in its **Global** profile therefore caps MoonSR's own presents. With a 30 fps global limit, 2X
  cannot be displayed: every real frame is shown late, the engine backs off to 1X ("Off (limited from 2X)"), and
  even at 1X each present blocks for a full limiter interval (measured 33 ms CPU per frame, 46 ms latency).
  This also explains the "72 presents/s cap" measured on 2026-11-13.
- Fix on the user's side: in RTSS add `MoonSR.exe` and set "Application detection level" to None (RTSS's own
  template for OBS does the same with `EnableHooking = 0`), or set the limit in the game's profile, not Global.
- MoonSR now detects `RTSSHooks64.dll` in its process, warns in the log and explains the fix in the status note.
- MoonSR does not exclude itself automatically (that would mean writing into RTSS's profile folder).

## Frame generation hand-over (2026-11-16, frame-generation-engineer; uncommitted)
- Jaggedness fixes: half-res hypothesis selection (fg_pixsel_cs), smooth motion upsampling + evidence-weighted
  boundary blending (fg_interpolate_ps), guided edge AA on generated frames only (fg_aa_ps). Search: near-best
  shortest-shift rule. Balanced: no propagation, 4 refine candidates, level-0 radius 1.
- Cost RTX 5060 1080p fs 0.5 (fgbench --time, idle GPU): Push/Interp ms P 1.14/0.30, B 1.25/0.41, Q 2.32/0.55.
  2X ceiling 1.45 ms: Performance meets (1.44), Balanced 1.66 and Quality 2.87 exceed. 4X ceiling 2.43: P, B meet.
- Open: Balanced still 0.2 ms over the 2X ceiling (HUD mask pass 0.25 ms is the next target); pixsel 0.2 ms;
  motion items 2-6 of research/ls_reference_motion_depth.md not done; thin moving structures (pillar edge) still
  have wrong motion; quality measured on synthetic and real-texture/synthetic-motion sequences only (no 60 fps
  real recording exists).

## GPU priority (2026-11-21)
- MoonSR now runs at GPU scheduling class REALTIME. Its work always goes ahead of the game's, so when MoonSR's own
  work needs more than the whole GPU (high multipliers or many NR passes on a small GPU) the game's frame rate drops
  instead of MoonSR's. Not verified: whether the desktop compositor or other windows stutter in that overload case.
- REALTIME was granted to an unelevated process on the development machine (Windows 11 26300). Other Windows builds
  may refuse it; MoonSR then falls back to HIGH and logs a warning. Under HAGS, HIGH gives no measurable benefit.
- Measured on NVIDIA (RTX 5060) only. AMD (AMDNR on its own D3D12 queue) and the RTX A400 are not measured.
- Residual NR slowdown under full load: about 0.85 ms per frame, in the optical-flow stage (driver-owned NVOFA queue).
- `moonsr_fgbench --selftest` fails "full-context comparison defers its second GPU submission". Pre-existing:
  the same failure occurs in the binary built at session start, before the priority change.

## Neural rendering on AMD and Intel (2026-11-21)
- Intel (DLSS5_INTEL) has not run on an Intel GPU: the development machine has none, and has neither the Vulkan SDK
  nor the extracted weights. Verified only with the stand-in runtime: bit-exact data path through the real bridge,
  settings, reset, resize, picture pass, and the Vulkan driver filter (with `*nv*` / `*amd*` patterns on this
  machine).
- Not verified: that Intel's Windows Vulkan driver manifest matches `*igvk*` (expected `igvk64.json`). If an Intel
  GPU is not found, set `MOONSR_INTEL_NR_DRIVERS` to a pattern matching its manifest file name.
- Intel throughput is far below real time (DLSS5_INTEL reports about 0.2 s per frame at 960x576 on an Arc Pro B50),
  and the runtime's C ABI forces a CPU readback and upload per frame.
- AMD: the bridge path passes with the AMD fixture on the Radeon 610M. The real AMDNR runtime cannot run on this
  machine: it needs AMD's HIP runtime (`amdhip64_7.dll`, not installed), and the 610M (RDNA 2) is outside AMDNR's
  supported GPUs (RX 7000, RX 9000, RDNA 3-class APUs).
