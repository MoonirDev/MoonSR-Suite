# MoonSR Suite

MoonSR brings upscaling, frame generation, and neural rendering (plus more to come) to any game, program or window.
It then shows the result in a borderless window at monitor resolution. It runs on Windows 10/11 and works across 
GPU vendors (NVIDIA, AMD, Intel). Lossless Scaling inspired the design. MoonSR does not inject anything into the 
game process.

It is an experimental personal project. Quality and speed claims come only from the measurements in
[documentation/BENCHMARKS.md](documentation/BENCHMARKS.md), with the hardware and settings stated there.

## Features

- **Capture:** Windows Graphics Capture of any window, with the cursor drawn in software.
- **Upscaling:** Nearest, Bilinear, Catmull-Rom, Lanczos-3, MoonSR Spatial (edge-adaptive) and MoonSR
  (temporal reconstruction from estimated motion; experimental), plus sharpening.
- **Frame generation:** 2X to 6X and an adaptive mode. Motion is estimated from the captured frames only. HUD
  protection and anti-aliasing apply to generated frames.
- **Neural rendering (optional):** runs a vendor neural-rendering model on the captured frames. You supply the
  runtime and model yourself:
  - NVIDIA: your DLSS Neural Rendering model (`nvngx_dlssnr.dll`).
  - AMD: the AMDNR runtime.
  - Intel: the DLSS5_INTEL runtime (experimental and far from real time).

  See [documentation/NR_BRIDGE.md](documentation/NR_BRIDGE.md).
- **GPU per stage:** on the Load order page, each stage (neural rendering, frame generation, upscaling) can run on
  its own GPU; frames are copied between GPUs where consecutive stages differ. NVIDIA neural rendering must share the
  upscaling GPU.
- **Scheduling:** MoonSR runs at the realtime GPU scheduling class, so its work goes ahead of the game's on a busy
  GPU.

Displayed frame rate includes generated frames. It is not the game's frame rate and does not reflect input
responsiveness.

## Requirements

- Windows 10 or 11. A Direct3D 11 GPU (feature level 11_0, Shader Model 5.0).
- To build: Visual Studio with the C++ desktop workload (developed with Visual Studio 2026, MSVC 14.52; C++20), CMake
  3.24+, Ninja and the Windows SDK (for `fxc`). `tools\build.ps1` finds Visual Studio with `vswhere`.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1                  # RelWithDebInfo into build\main
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Config Debug
```

Binaries are written to `build\main\bin`:

- `MoonSR.exe`: the application.
- `moonsr_cli.exe`: a scriptable driver.
- Test and benchmark tools: `moonsr_bench`, `moonsr_fgbench`, `moonsr_testpattern`, `moonsr_gpuload` and the smoke
  tests in `tests/`.

## Layout

| Path | Contents |
|---|---|
| `src/core`, `src/gpu`, `src/capture`, `src/present` | Types and logging; Direct3D devices and monitors; capture; output window |
| `src/upscale`, `src/framegen`, `src/neural` | Scalers and the MoonSR temporal upscaler; frame generation; host side of the neural rendering bridge |
| `src/engine` | Pipeline thread. `engine.h` is the only API the UI uses |
| `src/app` | User interface |
| `shaders/` | HLSL, compiled with `fxc` at build time and embedded |
| `sdk/include` | C ABI for neural rendering bridges |
| `addons/moonsr_nr_bridge` | Neural rendering bridge (NVIDIA, AMD and Intel backends) |
| `tools/`, `tests/`, `benchmarks/` | Measurement tools, smoke tests, reference implementations |
| `documentation/` | Architecture, benchmarks, known issues, research notes |

## Credits

- **MoonDev**: project owner. Defines the goals, tests on real games and makes the design decisions.
- **Claude** (Anthropic): wrote most of the code, shaders, tests and documentation with MoonDev, as an AI
  coding assistant in Claude Code. Commits it co-wrote carry a `Co-Authored-By: Claude` trailer.
- Third-party code and the projects it comes from are credited in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

MoonSR is released under the [MIT licence](LICENSE).

The exception is the neural rendering bridge (`addons/moonsr_nr_bridge/`, including the `neural_rendering/`
sources it compiles). It contains OptiScaler code and is therefore **GPL-3.0-only**. It builds as the separate `MoonSR_NrBridge.dll`, which MoonSR loads at run time.
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists every third-party component and its licence.

MoonSR includes and downloads no NVIDIA, AMD or Intel runtime, model or weights. DLSS is a trademark of NVIDIA
Corporation. MoonSR is not affiliated with NVIDIA, AMD, Intel or Lossless Scaling.
