// Persistent application settings (%LOCALAPPDATA%\MoonSR\settings.ini) via the Win32 private-profile APIs.
#pragma once

#include <string>
#include "neural/neural_settings.h"

namespace moonsr::app {

// A GPU choice, matched by vendorId + deviceId + name. All empty / zero = Auto (the GPU driving the output monitor).
struct GpuPref {
    unsigned vendorId = 0;
    unsigned deviceId = 0;
    std::wstring name;
    bool IsAuto() const { return vendorId == 0 && deviceId == 0 && name.empty(); }
};

// Indexes of AppSettings::stageGpu; the values match moonsr::PipelineStage.
enum StageGpuIndex { StageGpuNeural = 0, StageGpuFrameGen = 1, StageGpuUpscaling = 2, StageGpuCount = 3 };

struct AppSettings {
    // Source
    bool useForeground = true;
    // Output: monitors are matched by device name. Empty = Auto.
    std::wstring monitorDevice;
    // GPU per stage (the old single processing GPU is migrated into all three when reading an old settings.ini).
    GpuPref stageGpu[StageGpuCount];
    // Scaling
    bool upscaling = true;    // false = upscaling off: the image is shown 1:1 (frame generation can still run)
    std::wstring scalerName;  // empty = default
    int fitMode = 0;          // moonsr::FitMode
    int sharpnessPercent = 0; // 0..100
    bool vsync = false;
    bool showCursor = true;
    std::wstring theme = L"dark";   // [App] Theme: light | dark | retro or the file stem of an imported theme
    bool closeToTray = false;       // window close hides to the notification area instead of exiting
    bool showFrameCounter = false; // "<game fps>/<displayed fps>" in the top-left corner of the output
    // Frame generation
    bool frameGenEnabled = false;
    int frameGenMode = 0;         // 0 = Fixed (multiplier), 1 = Auto (target fps); moonsr::FrameGenMode
    int frameGenMultiplier = 1;   // Fixed multiplier in effect: 1 = off, 2..20 (kept for older readers)
    int frameGenLastMultiplier = 2;  // Fixed multiplier 2..20, restored when frame generation is switched back on
    int frameGenTargetFps = 120;  // Auto: desired frames per second, 30..1000
    int frameGenFlowPercent = 50; // 25..100 in steps of 5
    bool frameGenHudProtection = true;
    // Neural rendering (user-supplied model; see ModelStore)
    bool neuralRendering = false;
    std::wstring neuralModelId;
    NeuralSettings neuralSettings;
    PictureSettings pictureSettings;
    // MoonSR temporal upscaler. Keys that older versions wrote and this one no longer uses stay in the file and
    // are ignored when loading.
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
