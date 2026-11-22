# Third-party code and licences

MoonSR's own code is MIT-licensed (see [LICENSE](LICENSE)). The parts below keep their own licences.

## GPL-3.0-only: neural rendering bridge

| Path | Origin | Licence |
|---|---|---|
| `addons/moonsr_nr_bridge/` | MoonSR's neural rendering bridge. Built as the separate `MoonSR_NrBridge.dll`, which `MoonSR.exe` loads at run time through the C ABI in `sdk/include/moonsr_nr_bridge.h`. It compiles the GPL sources below, so it is GPL-3.0-only. | GPL-3.0-only, [LICENSE](addons/moonsr_nr_bridge/LICENSE) |
| `addons/moonsr_nr_bridge/neural_rendering/` | Subset of the Neural Rendering addon by FriedSnowman ([ls-addon-manager](https://github.com/FriedSnowman/ls-addon-manager), `addons/NeuralRendering`); includes `third_party/optiscaler/dlssnr.hlsl` from [OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) at `9737616` and a modified extraction of its forwarder (`src/model_forwarder.cpp`). Details in its [NOTICE](addons/moonsr_nr_bridge/neural_rendering/NOTICE.md). | GPL-3.0-only, [LICENSE](addons/moonsr_nr_bridge/neural_rendering/LICENSE); [OptiScaler GPL-3.0](addons/moonsr_nr_bridge/neural_rendering/licenses/OptiScaler-GPL-3.0.txt), [RenoDX attribution](addons/moonsr_nr_bridge/neural_rendering/licenses/RenoDX_ATTRIBUTION.txt) |

Anyone distributing `MoonSR_NrBridge.dll` must also offer its corresponding source (the `addons/moonsr_nr_bridge/`
folder and the build files), as GPL-3.0 requires. Distributing MoonSR without the bridge involves only the MIT terms.

## MIT

| Path | Origin |
|---|---|
| `addons/moonsr_nr_bridge/third_party/eam/real_frame.h` | Addon SDK header of [ls-addon-manager](https://github.com/FriedSnowman/ls-addon-manager) (`manager/sdk/include/eam`), by Echo-Storm on FrankBarretta's [LosslessProxy](https://github.com/FrankBarretta/LosslessProxy); [LICENSE](addons/moonsr_nr_bridge/third_party/eam/LICENSE) |
| `addons/moonsr_nr_bridge/neural_rendering/third_party/nvof/` | NVIDIA Optical Flow SDK headers, via [video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr) at `9e1bbfa`; NVIDIA's MIT notice is in each header; [video2dlssnr MIT](addons/moonsr_nr_bridge/neural_rendering/licenses/video2dlssnr-MIT.txt) |
| `addons/moonsr_nr_bridge/third_party/amd_nr/` | C ABI headers of lmxxf's AMDNR runtime (Copyright Kien), as used by [AMD-NR---OptiScaler](https://github.com/3zwr1/AMD-NR---OptiScaler); [LICENSE.lmxxf](addons/moonsr_nr_bridge/third_party/amd_nr/LICENSE.lmxxf) |

## Referenced, not included

- [DLSS5_INTEL](https://github.com/geladons/DLSS5_INTEL) (Apache-2.0): the Intel backend calls its `d5c_chain.dll`
  through function signatures declared in `addons/moonsr_nr_bridge/intel_model.h`. No code from it is included.
- Vendor runtimes and models (NVIDIA `nvngx_dlssnr.dll`, AMDNR `LmxxfNrRuntime`, DLSS5_INTEL weights) are supplied
  by the user and are never included or downloaded.
