// Verify persistent neural controls without writing to the user's actual settings.
#include "app/settings.h"
#include <windows.h>
#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || !SetEnvironmentVariableW(L"LOCALAPPDATA", argv[1])) return 2;
    moonsr::app::AppSettings saved;
    saved.neuralRendering = true;
    saved.neuralModelId = L"dlssneuralrendering-smoke";
    auto& n = saved.neuralSettings;
    n.passes = 20; n.resolutionPercent = 45; n.preset = 2; n.style = 1;
    n.intensityPercent = 135; n.structurePercent = 120; n.tonePercent = 55;
    n.skinPercent = 70; n.detailPercent = 205; n.colourPercent = 150; n.autoMask = false;
    auto& pic = saved.pictureSettings;
    pic.enabled = true; pic.exposure = -125; pic.contrast = 45; pic.midtones = -40;
    pic.shadows = 30; pic.highlights = -20; pic.saturation = 150; pic.vibrance = -50;
    pic.temperature = 75; pic.tint = -80; pic.sharpen = 220;
    if (!moonsr::app::SaveSettings(saved)) return 3;
    const auto read = moonsr::app::LoadSettings();
    const auto& r = read.neuralSettings;
    const auto& p = read.pictureSettings;
    const bool ok = read.neuralRendering && read.neuralModelId == saved.neuralModelId &&
        r.passes == 20 && r.resolutionPercent == 45 && r.preset == 2 && r.style == 1 &&
        r.intensityPercent == 135 && r.structurePercent == 120 && r.tonePercent == 55 &&
        r.skinPercent == 70 && r.detailPercent == 205 && r.colourPercent == 150 && !r.autoMask &&
        p.enabled && p.exposure == -125 && p.contrast == 45 && p.midtones == -40 && p.shadows == 30 &&
        p.highlights == -20 && p.saturation == 150 && p.vibrance == -50 && p.temperature == 75 &&
        p.tint == -80 && p.sharpen == 220;
    puts(ok ? "Neural settings persistence passed" : "Neural settings persistence failed");
    return ok ? 0 : 1;
}
