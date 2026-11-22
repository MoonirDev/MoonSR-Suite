// Persistent application settings (%LOCALAPPDATA%\MoonSR\settings.ini) via the Win32 private-profile APIs.
#pragma once

#include <string>
#include "neural/neural_settings.h"

namespace moonsr::app {

struct AppSettings {
    // Source
    bool useForeground = true;
    // Output: monitors are matched by device name, adapters by vendorId + deviceId + name. Empty / zero = Auto.
    std::wstring monitorDevice;
    unsigned adapterVendorId = 0;
    unsigned adapterDeviceId = 0;
    std::wstring adapterName;
    // Scaling
    bool upscaling = true;    // false = upscaling off: the image is shown 1:1 (frame generation can still run)
    std::wstring scalerName;  // empty = default
    int fitMode = 0;          // moonsr::FitMode
    int sharpnessPercent = 0; // 0..100
    bool vsync = false;
    bool showCursor = true;
    bool showFrameCounter = false; // "<game fps>/<displayed fps>" in the top-left corner of the output
    // Frame generation
    int frameGenMultiplier = 1;   // 1 = off, 2..6
    int frameGenLastMultiplier = 2;  // last non-off multiplier, restored when frame generation is switched back on
    int frameGenFlowPercent = 50; // 25..100 in steps of 5
    bool frameGenHudProtection = true;
    bool frameGenAdaptiveLearning = false;
    // Neural rendering (user-supplied model; see ModelStore)
    bool neuralRendering = false;
    std::wstring neuralModelId;
    NeuralSettings neuralSettings;
    PictureSettings pictureSettings;
    // Moon ALSR
    bool alsrLearning = true;
    bool alsrUseLearned = false;
    int alsrHistoryPercent = 85;  // 50..95
    // Processing order, e.g. "neural,framegen,upscaling"; empty / invalid = default
    std::wstring pipelineOrder;
    // Window position (top-left, virtual-desktop pixels)
    bool hasWindowPos = false;
    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;   // client size in DIPs; 0 = default
    int windowHeight = 0;
};

// %LOCALAPPDATA%\MoonSR, created on demand. Empty on failure.
std::wstring AppDataDirectory();

AppSettings LoadSettings();
bool SaveSettings(const AppSettings& settings);

} // namespace moonsr::app
