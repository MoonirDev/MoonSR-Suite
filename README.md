# MoonSR Suite

**Version 1.1.3**

MoonSR brings upscaling, frame generation, and neural rendering (plus more to come) to any game, program or window.
It then shows the result in a borderless window at monitor resolution. It runs on Windows 10/11 and works across 
GPU vendors (NVIDIA, AMD, Intel). Lossless Scaling inspired the design. MoonSR does not inject anything into the 
game process.

It is an experimental personal project. Quality and speed claims come only from the measurements in
[documentation/BENCHMARKS.md](documentation/BENCHMARKS.md), with the hardware and settings stated there.

## Features

- **Notices:** closed notices and warnings stay dismissed across restarts. Restore them from Settings.

- **Capture:** Windows Graphics Capture of any window, with the cursor drawn in software.
- **Upscaling:** Nearest, Bilinear, Catmull-Rom, Lanczos-3, MoonSR Spatial (edge-adaptive) and MoonSR
  (temporal reconstruction from estimated motion; experimental), plus sharpening.
- **Frame generation:** Fixed mode (2X to 20X) or Auto mode (type a target frame rate; MoonSR generates as many
  frames as needed, placing each at its true time between real frames). With V-Sync off it is not capped to the
  display's refresh rate. Motion is estimated from the captured frames only. HUD protection applies to generated
  frames.
- **Neural rendering (optional):** runs a vendor neural-rendering model on the captured frames. Supported cards:
  NVIDIA GeForce RTX, AMD Radeon RX 7000 and RX 9000 series, and Intel Arc A- and B-series (Intel is far from real
  time). You supply the runtime and model yourself; the Neural rendering page guides you through it:
  - NVIDIA: your DLSS Neural Rendering model (`nvngx_dlssnr.dll`).
  - AMD: the AMDNR runtime.
  - Intel: the DLSS5_INTEL runtime (experimental and far from real time).

  See [documentation/NR_BRIDGE.md](documentation/NR_BRIDGE.md).
- **GPU per stage:** on the Load order page, each stage (neural rendering, frame generation, upscaling, Smart parallax) can run on
  its own GPU; frames are copied between GPUs where consecutive stages differ. NVIDIA neural rendering on a second GPU
  runs in a small helper process (`MoonSR_NrHost.exe`).
- **Smart parallax (experimental):** adds surface relief to textured parts of the captured picture. Shading adds
  highlights and shadows. Parallax also shifts surface detail by up to 3 output pixels. Motion depth reuses compatible
  frame-generation motion and uses Parallax when that motion is unavailable. See the
  [Smart parallax design](documentation/research/screen_space_relief.md).
- **Themes:** Windows 11 Light, Windows 11 Dark, Retro and Retro Amber, plus your own `.moontheme` files
  ([documentation/THEMES.md](documentation/THEMES.md)). Optional transparency, glow and animations.
- **Notices and warnings:** close them with the × button. They briefly shrink and fade when visual effects are on.
- **Output window:** stays directly above the game, so other windows can be placed over it. MoonSR can close to the
  system tray.
- **Scheduling:** MoonSR runs at the realtime GPU scheduling class, so its work goes ahead of the game's on a busy
  GPU.

Displayed frame rate includes generated frames. It is not the game's frame rate and does not reflect input
responsiveness.

## Install

Download `MoonSR-Setup-<version>.exe` and run it. It installs for the current Windows user into
`%LOCALAPPDATA%\Programs\MoonSR` without an administrator prompt and needs no internet connection or extra runtime.

- **Updating:** run the newer installer. It updates the existing installation in place; your settings, themes and
  models in `%LOCALAPPDATA%\MoonSR` are kept. Close MoonSR first (the installer asks if it is running).
- **Uninstalling:** Windows Settings > Apps > MoonSR, or the uninstaller in the Start menu. It removes the program
  and any neural rendering runtimes you imported through MoonSR, and asks whether to also delete your settings
  (default: keep them).
- The installer is not code-signed yet, so Windows SmartScreen may warn the first time.

### No internet connection, ever

MoonSR never connects to the internet: no update checks, telemetry, analytics, downloads or accounts. It will not
gain any such feature. Links in the app (for example to this page) open in your own web browser; MoonSR itself sends
nothing. Neural rendering runtimes you install yourself (NVIDIA, AMD or Intel) are third-party software that MoonSR
loads but does not control.

**Updates are manual.** Settings > Application has a link that opens the releases page in your browser; download
the newer installer and run it. Your settings are kept.

## Requirements

- Windows 10 or 11. A Direct3D 11 GPU (feature level 11_0, Shader Model 5.0).
- To build: Visual Studio with the C++ desktop workload (developed with Visual Studio 2026, MSVC 14.52; C++20), CMake
  3.24+, Ninja and the Windows SDK (for `fxc`). `tools\build.ps1` finds Visual Studio with `vswhere`.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1                  # RelWithDebInfo into build\main
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Config Debug
```

To build the installer (needs [Inno Setup 6](https://jrsoftware.org/isinfo.php)):

```powershell
powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1   # Release build, then dist\MoonSR-Setup-<version>.exe
```

The version is set in one place: the root `CMakeLists.txt` (`project(MoonSR VERSION x.y.z)`).

Binaries are written to `build\main\bin`:

- `MoonSR.exe`: the application.
- `moonsr_cli.exe`: a scriptable driver.
- Test and benchmark tools: `moonsr_bench`, `moonsr_fgbench`, `moonsr_testpattern`, `moonsr_gpuload` and the smoke
  tests in `tests/`.

## Releases

- **1.1.3** (2026-11-25): Notices and warnings now stay closed across restarts. Added a Settings switch to restore dismissed messages; closing animations are kept.

- **1.1.2** (2026-11-25): improves Smart parallax around edges and during frame generation, reduces false pauses
  from grain, and explains when Motion depth falls back to Parallax.
- **1.1.1** (2026-11-24): restores the close button on notices and warnings, including the short closing animation.
- **1.1.0** (2026-11-23): adds experimental Smart parallax with Shading, Parallax and Motion depth. Smart parallax
  can move in Load order and run on a GPU of its own. Motion depth works with frame generation off. Home adds an
  A/B comparison of the raw capture and the finished image.
- **1.0.2** (2026-11-22): fixes. Frame generation with neural rendering and V-Sync on no longer drops to about 95 fps
  (60 fps game, 2X: now 120 fps, the same as with V-Sync off); the output window uses three buffers. The neural
  rendering setup now requires the FP16-patched model on RTX 20, 30 and 40 series cards (identified by hardware ID,
  including professional RTX cards) and explains it when the standard model is selected.
- **1.0.1** (2026-11-22): guided neural rendering setup on the Neural rendering page (status per GPU, the next step for
  each, and Import buttons for `nvngx_dlssnr.dll`, the AMDNR release zip or files, and the DLSS5_INTEL bundle and
  weights); clearer supported-card wording; an Updates row in Settings with a link to the releases page; uninstalling
  now also removes runtimes imported through MoonSR.
- **1.0.0** (2026-11-22): first versioned release with an installer. Frame generation Fixed (2X-20X) and Auto
  (target frame rate) modes; no refresh-rate cap with V-Sync off; neural rendering on a GPU other than the one driving the monitor; themes with custom
  theme import; new logo; close to tray; groundwork for Smart parallax; output window that other windows can cover; online learning removed and the
  temporal upscaler renamed MoonSR.

## Layout

| Path | Contents |
|---|---|
| `src/core`, `src/gpu`, `src/capture`, `src/present` | Types and logging; Direct3D devices and monitors; capture; output window |
| `src/upscale`, `src/framegen`, `src/neural` | Scalers and the MoonSR temporal upscaler; frame generation; host side of the neural rendering bridge |
| `src/relief` | Smart parallax stage |
| `src/engine` | Pipeline thread. `engine.h` is the only API the UI uses |
| `src/app` | User interface |
| `shaders/` | HLSL, compiled with `fxc` at build time and embedded |
| `sdk/include` | C ABI for neural rendering bridges |
| `addons/moonsr_nr_bridge` | Neural rendering bridge (NVIDIA, AMD and Intel backends) |
| `tools/`, `tests/`, `benchmarks/` | Measurement tools, smoke tests, reference implementations |
| `installer/` | Inno Setup script for the Windows installer |
| `documentation/` | Architecture, benchmarks, known issues, research notes |

## Credits

- **MoonDev**: project owner. Defines the goals, tests on real games and makes the design decisions.
- **Claude** (Anthropic): wrote most of the code, shaders, tests and documentation with MoonDev, as an AI
  coding assistant in Claude Code. Commits it co-wrote carry a `Co-Authored-By: Claude` trailer.
- **ChatGPT Astra** (OpenAI): contributed code to MoonSR as an AI coding assistant.
- Third-party code and the projects it comes from are credited in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

MoonSR is released under the [MIT licence](LICENSE).

The exception is the neural rendering bridge (`addons/moonsr_nr_bridge/`, including the `neural_rendering/`
sources it compiles). It contains OptiScaler code and is therefore **GPL-3.0-only**. It builds as the separate `MoonSR_NrBridge.dll`, which MoonSR loads at run time.
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists every third-party component and its licence.

MoonSR includes and downloads no NVIDIA, AMD or Intel runtime, model or weights. DLSS is a trademark of NVIDIA
Corporation. MoonSR is not affiliated with NVIDIA, AMD, Intel or Lossless Scaling.
