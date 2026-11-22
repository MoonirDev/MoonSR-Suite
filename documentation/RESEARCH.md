# MoonSR Suite — Research Summary

Consolidated by the lead on 2026-11-13 from four research reports. This file holds conclusions and the comparison
matrix. Evidence, per-claim verification tags and source URLs are in the detailed notes:

- `research/nvidia.md` — DLSS SR / RR / FG / MFG / Dynamic MFG, NIS, Reflex, Smooth Motion, NvOFFRUC
- `research/amd_intel.md` — FSR 1–4, Redstone, AFMF, FidelityFX optical flow and pacing, XeSS, XeLL
- `research/capture_tools_and_algorithms.md` — Lossless Scaling and other capture-based tools, interpolation and
  upscaling algorithms, licences
- `research/windows_capture_presentation.md` — WGC, Desktop Duplication, overlay window, swapchain, cursor, multi-GPU
- `research/alsr_v2.md` — MoonSR v2 audit and plan (target XeSS 2 / FSR 4-class quality at the current cost, learning removed)
- `research/fg_quality.md` — frame generation base-quality audit and plan

The researchers tagged every claim as verified / press-only / inferred / from memory / unknown. Performance figures
quoted in those notes are third-party or estimates. None are MoonSR measurements; those live in `BENCHMARKS.md`.

## 1. The central constraint

MoonSR sees only the finished frame. Every engine-integrated technology (DLSS, FSR 2/3/4 upscaling, XeSS SR, and all
vendor frame generation except driver-level tools) requires motion vectors, depth and jitter, usually a HUD-less
image and control of the game's swapchain. **None of them can run on a captured window**, technically or under their
licences. They are quality references, not components.

Consequence for the project goals: a comparison "against DLSS 4.5 at equal input resolution" is only meaningful as
image-vs-ground-truth measurement on scenes where a DLSS output can be recorded. A capture-based upscaler starts
from an image that already had TAA applied and has less information than DLSS receives. Matching it is a research
goal that must be shown by measurement, and may not be reachable on the universal path; the integration bridge
(engine data supplied by a mod/addon) is the route to parity.

## 2. Comparison matrix

| Technology | Kind | Inputs needed | Works on a captured image | Hardware | Licence / reuse | Use for MoonSR |
|---|---|---|---|---|---|---|
| DLSS SR / DLAA (CNN → transformer in DLSS 4 / 4.5) | temporal ML upscaler | colour, MVs, depth, jitter, exposure | No | NVIDIA RTX | RTX SDK licence, NVIDIA GPUs only | Quality reference |
| DLSS Ray Reconstruction | ML denoiser+upscaler | G-buffer data | No | RTX | same | None |
| DLSS FG / MFG / Dynamic MFG | ML interpolation, 2X–6X | MVs, depth, HUD-less, UI, camera, Reflex, swapchain | No | RTX 40/50 (MFG: 50) | same | Design lessons (§4) |
| NVIDIA Smooth Motion | driver-level FG, 2X | presented frames | n/a (no API) | RTX 40/50 | closed | Proof that capture-class FG is viable |
| NvOFFRUC (Optical Flow SDK) | 2-frame interpolation | two frames | Yes | NVIDIA Turing+ | DesignWorks licence | Possible optional NVIDIA-only backend; not baseline |
| NVIDIA Image Scaling | spatial scaler + sharpen | colour | Yes | any | MIT | Reference to beat; could ship as optional scaler |
| FSR 1 (EASU + RCAS), CAS | spatial scaler / sharpen | colour | Yes | any | per-file check required (see §5) | Reference to beat |
| FSR 2 / 3 upscaler | temporal analytic | MVs, depth, jitter, reactive | No | any | FidelityFX SDK | Ideas for temporal stage (locks, disocclusion) |
| FSR 4 / Redstone | ML upscaler / ML FG | engine data | No | recent Radeon | proprietary parts | None |
| FidelityFX Optical Flow | pyramidal block matching | colour only | Yes (SM 6.2) | any DX12 | per-file check | **Blueprint** for our own SM 5.0 flow (clean-room) |
| FSR 3 frame interpolation | analytic FG | game MVs + depth + optical flow | Not as documented | any DX12 | per-file check | Pacing-thread lessons |
| AMD AFMF | driver-level FG | presented frames | n/a (driver) | Radeon | closed | Artifact classes to test against |
| XeSS SR / FG / MFG, XeLL | ML upscaler / FG | engine data | No | cross-vendor SR; FG varies | Intel Simplified SL, binary only | None |
| Lossless Scaling (LS1, LSFG 3) | capture-based scaler + FG up to x20, adaptive | captured frames | Yes | D3D11, incl. iGPU | proprietary | **Primary functional and performance reference** |
| Game Frame, DraFG | capture-based FG | captured frames | Yes | varies | proprietary | Competitors to track |
| lsfg-vk | LSFG on Linux | LS files | — | — | CC BY-NC-ND | **Do not use or derive from** |
| Magpie | capture-based scaler | captured frames | Yes | D3D11 | GPLv3 | Read for behaviour only; **no code reuse** |
| RIFE / IFRNet | neural interpolation | two frames | Yes | mid/high GPU | MIT (weights trained on Vimeo-90K: data rights unclear) | Architecture ideas for high-end tier; train own weights |
| ArtCNN, Anime4K | small CNN upscalers | colour | Yes | any (shader) | MIT | Model class for neural refinement stage |
| xBRZ / hqx / CuNNy / RAVU | pixel-art / prescalers | colour | Yes | any | GPL / LGPL | Not without legal review |
| "Apex Compute ANR FG" | — | — | — | — | — | **Not found in any public source.** Not a target until a reference exists |

## 3. Decisions taken from the research

1. **Clean-room everything in the baseline.** The only clearly reusable third-party pieces are MIT-licensed (NIS,
   ArtCNN, Anime4K, RIFE code). They may become optional scalers/addons later; the default path is original code.
2. **Upscaler stack** (in order): original edge-adaptive spatial scaler + sharpen → tiny CNN refinement compiled to
   HLSL → temporal stabilisation reusing the frame-generation flow field → content modes (integer, pixel art,
   anime). Transformer-class models are out of reach for the hardware floor; even NVIDIA's cost heavily on RTX
   20/30. Jitterless temporal super-resolution from a captured stream has limited upside because the game already
   applied TAA; the temporal stage is primarily for stability.
3. **Frame generation stack**: pyramidal block-matching optical flow written for SM 5.0 compute, run at a reduced
   "flow scale", once per real-frame pair and bidirectional; confidence from forward-backward consistency;
   interpolation by warping both real frames to time t and blending by confidence; scene-cut detection from
   region histograms; static-HUD detection. Midrange adds census cost and sub-pixel refinement. High-end adds an
   optional neural model with our own weights.
4. **Multi-frame generation**: heavy work once per real pair, light work per generated frame, each timestep from
   the original pair. This is both NVIDIA's documented cost structure and the fix for error accumulation.
5. **Pacing is its own subsystem**: dedicated high-priority present thread, evenly spaced targets, final
   busy-wait; always present real frames, drop late generated ones; on low confidence repeat instead of showing a
   broken frame. Adaptive multiplier with hysteresis and a minimum hold time.
6. **Capture / presentation**: WGC, free-threaded pool, newest-frame-wins, `MinUpdateInterval` (24H2+) for >60 fps,
   client-area crop; overlay `WS_POPUP` + `WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP`,
   click-through, excluded from capture; `FLIP_DISCARD` + waitable object + tearing. Swapchain on the adapter that
   owns the monitor. Cursor: hide the system cursor and draw a mapped software cursor (Phase 2 follow-up).
7. **No injection** and no undocumented DWM surfaces in the default path.
8. **Process in linear light** eventually (DLSS 4.5 moved to linear for quality); today the pipeline is gamma-space.

## 4. Open questions for the project owner

- "Apex Compute ANR FG": please supply a link or the correct product name.
- Will MoonSR be open source or closed? It affects whether optional vendor binaries (e.g. NvOFFRUC) could ever be
  bundled, and which licences are acceptable for optional scalers.
- Is shipping MIT-licensed third-party scalers (NIS, Anime4K) as optional choices acceptable, clearly labelled as
  such and separate from the "custom" technology?

## 5. Licence cautions

- AMD FidelityFX SDK: reported as a proprietary default licence with an MIT carve-out for a file list. Nothing is
  reused until file paths are matched against that list by a human.
- XeSS: binary redistribution only, no modification or reverse engineering.
- DLSS: NVIDIA GPUs only, clause concerning open-source licensing.
- Neural weights trained on Vimeo-90K carry unclear commercial rights; train on self-rendered data.
- GPL / LGPL / CC-NC-ND sources listed in §2 are excluded.

## 6. Not verified (carried over from the notes)

WGC added latency in DWM intervals; whether WGC capture disables independent flip for the game; window-style
effects on flip eligibility; VRR behaviour of windowed flip-model overlays; Lossless Scaling internals; AFMF and
Smooth Motion algorithms; DLSS per-GPU timings; Dynamic MFG's selection rule; EASU/RCAS constants (not needed for a
clean-room design).

## 7. Corrections and additions (2026-11-15)

- **Section 1 and the matrix were too strong about DLSS.** DLSS Super Resolution / DLAA *can* be run on captured
  frames by supplying stand-in inputs (image-based motion vectors, constant depth, zero jitter); the public SDK
  supports this on D3D11 and D3D12. Quality is limited by the missing jitter and depth. The same correction applies
  to `research/nvidia.md` section 1.7.
- **DLSS 5 Neural Rendering** is not exposed by NVIDIA's public SDKs (feature id 18 is "Reserved18" in the public
  header; Streamline 2.14 names an `sl.dlss_nr` plugin but ships no header or guide). Community projects drive
  `nvngx_dlssnr.dll` directly with definitions from OptiScaler. Requirements stated by NVIDIA: RTX 50 class,
  driver 616.64+. Details: `research/dlss_capture_integration.md`.
- **Project status changed:** personal use only, not distributed (see CLAUDE.md). Licence compatibility is no
  longer a design constraint; sections 3.1 and 5 are kept for the record only.
- FriedSnowman/ls-addon-manager is the reference for running vendor models on captures and for feature ideas
  (per-game settings, before/after comparison, runtimes list, diagnostics bundle, load order).
