// Settings shared between the launcher, engine and neural bridge host.
#pragma once
#include <cstdint>

namespace moonsr {
// Picture controls are stored in hundredths; exposure is in stops.
struct PictureSettings {
    bool enabled = false;
    int32_t exposure = 0, contrast = 0, midtones = 0, shadows = 0, highlights = 0;
    int32_t saturation = 100, vibrance = 0, temperature = 0, tint = 0, sharpen = 0;
};
struct NeuralSettings {
    uint32_t passes = 1;
    uint32_t resolutionPercent = 100;
    uint32_t preset = 0; // 0 DLL default, 1 Original, 2 Fast
    uint32_t style = 0;  // 0 Default, 1 Natural, 2 Cinematic
    uint32_t intensityPercent = 100;
    uint32_t structurePercent = 80;
    uint32_t tonePercent = 30;
    uint32_t skinPercent = 80;
    uint32_t detailPercent = 100;
    uint32_t colourPercent = 100;
    bool autoMask = true;
    bool operator==(const NeuralSettings&) const = default;
};
}
