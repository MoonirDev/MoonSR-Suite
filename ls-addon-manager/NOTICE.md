# ls-addon-manager (subset)

This folder holds the parts of the Addon Manager for Lossless Scaling source that MoonSR's neural rendering bridge
(`addons/moonsr_nr_bridge`) compiles. Everything else from that project (the manager, installer, Upscalers addon,
tools and documentation) was removed because MoonSR does not use it.

| Part | Author | Licence |
|---|---|---|
| `manager/sdk/include/eam/real_frame.h` | Echo-Storm, on FrankBarretta's [LosslessProxy](https://github.com/FrankBarretta/LosslessProxy) | MIT, [LICENSE](LICENSE) |
| `addons/NeuralRendering/` | FriedSnowman, with pinned OptiScaler and RenoDX-derived composition; see its [NOTICE](addons/NeuralRendering/NOTICE.md) | GPL-3.0-only, [LICENSE](addons/NeuralRendering/LICENSE) |

The original copyright and licence in [LICENSE](LICENSE) must stay with every copy.
