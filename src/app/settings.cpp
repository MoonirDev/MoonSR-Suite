#include "settings.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <string>

namespace moonsr::app {
namespace {

std::wstring SettingsPath() {
    const std::wstring dir = AppDataDirectory();
    return dir.empty() ? std::wstring() : dir + L"\\settings.ini";
}

// The private-profile APIs store text as ANSI unless the file already starts with a UTF-16 BOM.
// Creating the file with a BOM keeps device and adapter names intact.
void EnsureUnicodeFile(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(f, &bom, sizeof(bom), &written, nullptr);
    CloseHandle(f);
}

std::wstring ReadString(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(section, key, L"", buf, static_cast<DWORD>(std::size(buf)), path.c_str());
    return buf;
}

int ReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int def) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, static_cast<INT>(def), path.c_str()));
}

// GetPrivateProfileIntW parses negative numbers as 0, which breaks window positions left of / above the primary
// monitor. Read the text and parse it with wcstol instead.
int ReadSignedInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int def) {
    const std::wstring text = ReadString(path, section, key);
    if (text.empty()) return def;
    wchar_t* end = nullptr;
    const long v = wcstol(text.c_str(), &end, 10);
    if (end == text.c_str()) return def;
    return static_cast<int>(std::clamp<long>(v, -1000000, 1000000));
}

bool WriteString(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    return WritePrivateProfileStringW(section, key, value.c_str(), path.c_str()) != FALSE;
}

bool WriteInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, long long value) {
    return WriteString(path, section, key, std::to_wstring(value));
}

} // namespace

std::wstring AppDataDirectory() {
    wchar_t base[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const std::wstring dir = std::wstring(base) + L"\\MoonSR";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
    return dir;
}

AppSettings LoadSettings() {
    AppSettings s;
    const std::wstring path = SettingsPath();
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return s;

    s.useForeground = ReadInt(path, L"Source", L"UseForeground", 1) != 0;
    s.monitorDevice = ReadString(path, L"Output", L"MonitorDevice");
    {
        auto read = [&](const wchar_t* section, const wchar_t* prefix) {
            const std::wstring p = prefix;
            GpuPref g;
            g.vendorId = static_cast<unsigned>(ReadInt(path, section, (p + L"VendorId").c_str(), 0));
            g.deviceId = static_cast<unsigned>(ReadInt(path, section, (p + L"DeviceId").c_str(), 0));
            g.name = ReadString(path, section, (p + L"Name").c_str());
            return g;
        };
        if (ReadInt(path, L"Gpu", L"PerStage", 0) != 0) {
            static const wchar_t* const kPrefix[StageGpuCount] = {L"Neural", L"FrameGen", L"Upscaling", L"Relief"};
            for (int i = 0; i < StageGpuCount; ++i) s.stageGpu[i] = read(L"Gpu", kPrefix[i]);
        } else {
            // Older file: one processing GPU for everything.
            const GpuPref legacy = read(L"Output", L"Adapter");
            for (GpuPref& g : s.stageGpu) g = legacy;
        }
    }
    s.upscaling = ReadInt(path, L"Scaling", L"Enabled", 1) != 0;
    s.scalerName = ReadString(path, L"Scaling", L"Scaler");
    s.fitMode = std::clamp(ReadInt(path, L"Scaling", L"FitMode", 0), 0, 2);
    s.sharpnessPercent = std::clamp(ReadInt(path, L"Scaling", L"Sharpness", 0), 0, 100);
    s.vsync = ReadInt(path, L"Scaling", L"VSync", 0) != 0;
    s.showCursor = ReadInt(path, L"Scaling", L"ShowCursor", 1) != 0;
    s.closeToTray = ReadInt(path, L"App", L"CloseToTray", 0) != 0;
    s.neuralSetupCollapsed = ReadInt(path, L"Neural", L"SetupCollapsed", 0) != 0;
    s.visualEffects = ReadInt(path, L"App", L"VisualEffects", 1) != 0;
    s.theme = ReadString(path, L"App", L"Theme");
    if (s.theme.empty()) s.theme = L"dark";
    s.showFrameCounter = ReadInt(path, L"Scaling", L"ShowFrameCounter", 0) != 0;
    s.neuralRendering = ReadInt(path, L"Neural", L"Enabled", 0) != 0;
    s.neuralModelId = ReadString(path, L"Neural", L"ModelId");
    auto& nr = s.neuralSettings;
    nr.passes = std::clamp(ReadSignedInt(path, L"Neural", L"Passes", 1), 1, 20);
    nr.resolutionPercent = std::clamp(ReadSignedInt(path, L"Neural", L"Resolution", 100), 25, 100);
    nr.preset = std::clamp(ReadSignedInt(path, L"Neural", L"Preset", 0), 0, 2);
    nr.style = std::clamp(ReadSignedInt(path, L"Neural", L"Style", 0), 0, 2);
    nr.intensityPercent = std::clamp(ReadSignedInt(path, L"Neural", L"Intensity", 100), 0, 200);
    nr.structurePercent = std::clamp(ReadSignedInt(path, L"Neural", L"Structure", 80), 0, 200);
    nr.tonePercent = std::clamp(ReadSignedInt(path, L"Neural", L"Tone", 30), 0, 200);
    nr.skinPercent = std::clamp(ReadSignedInt(path, L"Neural", L"Skin", 80), 0, 200);
    nr.detailPercent = std::clamp(ReadSignedInt(path, L"Neural", L"Detail", 100), 0, 400);
    nr.colourPercent = std::clamp(ReadSignedInt(path, L"Neural", L"Colour", 100), 0, 400);
    nr.autoMask = ReadSignedInt(path, L"Neural", L"AutoSkin", 1) != 0;
    auto& pic = s.pictureSettings;
    pic.enabled = ReadSignedInt(path, L"Picture", L"Enabled", 0) != 0;
    pic.exposure = std::clamp(ReadSignedInt(path, L"Picture", L"Exposure", 0), -400, 400);
    pic.contrast = std::clamp(ReadSignedInt(path, L"Picture", L"Contrast", 0), -200, 200);
    pic.midtones = std::clamp(ReadSignedInt(path, L"Picture", L"Midtones", 0), -200, 200);
    pic.shadows = std::clamp(ReadSignedInt(path, L"Picture", L"Shadows", 0), -200, 200);
    pic.highlights = std::clamp(ReadSignedInt(path, L"Picture", L"Highlights", 0), -200, 200);
    pic.saturation = std::clamp(ReadSignedInt(path, L"Picture", L"Saturation", 100), 0, 400);
    pic.vibrance = std::clamp(ReadSignedInt(path, L"Picture", L"Vibrance", 0), -100, 200);
    pic.temperature = std::clamp(ReadSignedInt(path, L"Picture", L"Temperature", 0), -200, 200);
    pic.tint = std::clamp(ReadSignedInt(path, L"Picture", L"Tint", 0), -200, 200);
    pic.sharpen = std::clamp(ReadSignedInt(path, L"Picture", L"Sharpen", 0), 0, 400);
    s.alsrHistoryPercent = std::clamp(ReadSignedInt(path, L"Alsr", L"HistoryPercent", 85), 50, 95);
    s.reliefEnabled = ReadSignedInt(path, L"Relief", L"Enabled", 0) != 0;
    s.reliefMode = std::clamp(ReadSignedInt(path, L"Relief", L"Mode", 0), 0, 2);
    s.reliefStrengthPercent = std::clamp(ReadSignedInt(path, L"Relief", L"StrengthPercent", 50), 0, 100);
    s.reliefParallaxHundredths = std::clamp(ReadSignedInt(path, L"Relief", L"ParallaxPx100", 200), 0, 300);
    s.reliefDetailHundredths = std::clamp(ReadSignedInt(path, L"Relief", L"DetailPx100", 200), 75, 400);
    s.reliefSplit = ReadSignedInt(path, L"Relief", L"SplitView", 0) != 0;
    s.pipelineOrder = ReadString(path, L"Pipeline", L"Order");
    s.hasWindowPos = ReadInt(path, L"Window", L"HasPosition", 0) != 0;
    s.windowX = ReadSignedInt(path, L"Window", L"X", 0);
    s.windowY = ReadSignedInt(path, L"Window", L"Y", 0);
    s.windowWidth = std::clamp(ReadSignedInt(path, L"Window", L"Width", 0), 0, 10000);
    s.windowHeight = std::clamp(ReadSignedInt(path, L"Window", L"Height", 0), 0, 10000);
    // Older files: Multiplier 1 = off, 2..6 = on (no Enabled / Mode / TargetFps keys); they map over unchanged.
    s.frameGenMultiplier = std::clamp(ReadSignedInt(path, L"FrameGen", L"Multiplier", 1), 1, 20);
    s.frameGenLastMultiplier =
        std::clamp(ReadSignedInt(path, L"FrameGen", L"LastMultiplier", s.frameGenMultiplier > 1 ? s.frameGenMultiplier : 2), 2, 20);
    s.frameGenEnabled = ReadSignedInt(path, L"FrameGen", L"Enabled", s.frameGenMultiplier > 1 ? 1 : 0) != 0;
    s.frameGenMode = std::clamp(ReadSignedInt(path, L"FrameGen", L"Mode", 0), 0, 1);
    s.frameGenTargetFps = std::clamp(ReadSignedInt(path, L"FrameGen", L"TargetFps", 120), 30, 1000);
    const int flow = std::clamp(ReadSignedInt(path, L"FrameGen", L"FlowScalePercent", 50), 25, 100);
    s.frameGenFlowPercent = std::clamp(((flow + 2) / 5) * 5, 25, 100);
    s.frameGenHudProtection = ReadSignedInt(path, L"FrameGen", L"HudProtection", 1) != 0;
    return s;
}

bool SaveSettings(const AppSettings& s) {
    const std::wstring path = SettingsPath();
    if (path.empty()) return false;
    EnsureUnicodeFile(path);

    bool ok = true;
    ok &= WriteInt(path, L"Source", L"UseForeground", s.useForeground ? 1 : 0);
    ok &= WriteString(path, L"Output", L"MonitorDevice", s.monitorDevice);
    ok &= WriteInt(path, L"Gpu", L"PerStage", 1);
    {
        static const wchar_t* const kPrefix[StageGpuCount] = {L"Neural", L"FrameGen", L"Upscaling", L"Relief"};
        for (int i = 0; i < StageGpuCount; ++i) {
            const std::wstring p = kPrefix[i];
            ok &= WriteInt(path, L"Gpu", (p + L"VendorId").c_str(), s.stageGpu[i].vendorId);
            ok &= WriteInt(path, L"Gpu", (p + L"DeviceId").c_str(), s.stageGpu[i].deviceId);
            ok &= WriteString(path, L"Gpu", (p + L"Name").c_str(), s.stageGpu[i].name);
        }
    }
    ok &= WriteInt(path, L"Scaling", L"Enabled", s.upscaling ? 1 : 0);
    ok &= WriteString(path, L"Scaling", L"Scaler", s.scalerName);
    ok &= WriteInt(path, L"Scaling", L"FitMode", s.fitMode);
    ok &= WriteInt(path, L"Scaling", L"Sharpness", s.sharpnessPercent);
    ok &= WriteInt(path, L"Scaling", L"VSync", s.vsync ? 1 : 0);
    ok &= WriteInt(path, L"Scaling", L"ShowCursor", s.showCursor ? 1 : 0);
    ok &= WriteInt(path, L"App", L"CloseToTray", s.closeToTray ? 1 : 0);
    ok &= WriteInt(path, L"Neural", L"SetupCollapsed", s.neuralSetupCollapsed ? 1 : 0);
    ok &= WriteInt(path, L"App", L"VisualEffects", s.visualEffects ? 1 : 0);
    ok &= WriteString(path, L"App", L"Theme", s.theme);
    ok &= WriteInt(path, L"Scaling", L"ShowFrameCounter", s.showFrameCounter ? 1 : 0);
    ok &= WriteInt(path, L"FrameGen", L"Enabled", s.frameGenEnabled ? 1 : 0);
    ok &= WriteInt(path, L"FrameGen", L"Mode", s.frameGenMode);
    ok &= WriteInt(path, L"FrameGen", L"Multiplier", s.frameGenMultiplier);
    ok &= WriteInt(path, L"FrameGen", L"TargetFps", s.frameGenTargetFps);
    ok &= WriteInt(path, L"FrameGen", L"LastMultiplier", s.frameGenLastMultiplier);
    ok &= WriteInt(path, L"FrameGen", L"FlowScalePercent", s.frameGenFlowPercent);
    ok &= WriteInt(path, L"FrameGen", L"HudProtection", s.frameGenHudProtection ? 1 : 0);
    ok &= WriteInt(path, L"Neural", L"Enabled", s.neuralRendering ? 1 : 0);
    ok &= WriteString(path, L"Neural", L"ModelId", s.neuralModelId);
    const auto& nr = s.neuralSettings;
    ok &= WriteInt(path, L"Neural", L"Passes", nr.passes);
    ok &= WriteInt(path, L"Neural", L"Resolution", nr.resolutionPercent);
    ok &= WriteInt(path, L"Neural", L"Preset", nr.preset);
    ok &= WriteInt(path, L"Neural", L"Style", nr.style);
    ok &= WriteInt(path, L"Neural", L"Intensity", nr.intensityPercent);
    ok &= WriteInt(path, L"Neural", L"Structure", nr.structurePercent);
    ok &= WriteInt(path, L"Neural", L"Tone", nr.tonePercent);
    ok &= WriteInt(path, L"Neural", L"Skin", nr.skinPercent);
    ok &= WriteInt(path, L"Neural", L"Detail", nr.detailPercent);
    ok &= WriteInt(path, L"Neural", L"Colour", nr.colourPercent);
    ok &= WriteInt(path, L"Neural", L"AutoSkin", nr.autoMask ? 1 : 0);
    const auto& pic = s.pictureSettings;
    ok &= WriteInt(path, L"Picture", L"Enabled", pic.enabled ? 1 : 0);
    ok &= WriteInt(path, L"Picture", L"Exposure", pic.exposure);
    ok &= WriteInt(path, L"Picture", L"Contrast", pic.contrast);
    ok &= WriteInt(path, L"Picture", L"Midtones", pic.midtones);
    ok &= WriteInt(path, L"Picture", L"Shadows", pic.shadows);
    ok &= WriteInt(path, L"Picture", L"Highlights", pic.highlights);
    ok &= WriteInt(path, L"Picture", L"Saturation", pic.saturation);
    ok &= WriteInt(path, L"Picture", L"Vibrance", pic.vibrance);
    ok &= WriteInt(path, L"Picture", L"Temperature", pic.temperature);
    ok &= WriteInt(path, L"Picture", L"Tint", pic.tint);
    ok &= WriteInt(path, L"Picture", L"Sharpen", pic.sharpen);
    ok &= WriteInt(path, L"Alsr", L"HistoryPercent", s.alsrHistoryPercent);
    ok &= WriteInt(path, L"Relief", L"Enabled", s.reliefEnabled ? 1 : 0);
    ok &= WriteInt(path, L"Relief", L"Mode", s.reliefMode);
    ok &= WriteInt(path, L"Relief", L"StrengthPercent", s.reliefStrengthPercent);
    ok &= WriteInt(path, L"Relief", L"ParallaxPx100", s.reliefParallaxHundredths);
    ok &= WriteInt(path, L"Relief", L"DetailPx100", s.reliefDetailHundredths);
    ok &= WriteInt(path, L"Relief", L"SplitView", s.reliefSplit ? 1 : 0);
    ok &= WriteString(path, L"Pipeline", L"Order", s.pipelineOrder);
    ok &= WriteInt(path, L"Window", L"HasPosition", s.hasWindowPos ? 1 : 0);
    ok &= WriteInt(path, L"Window", L"X", s.windowX);
    ok &= WriteInt(path, L"Window", L"Y", s.windowY);
    ok &= WriteInt(path, L"Window", L"Width", s.windowWidth);
    ok &= WriteInt(path, L"Window", L"Height", s.windowHeight);
    return ok;
}

} // namespace moonsr::app
