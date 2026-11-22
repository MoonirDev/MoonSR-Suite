// MoonSR main window: window procedure, navigation host and engine wiring. The visuals live in ui/ and pages.cpp.
// Threading: UI thread only; the engine is only used through engine.h and never blocks the UI thread except in
// Engine::Start / Engine::Stop (as before).
#pragma once

#include "engine/engine.h"
#include "pages.h"
#include "settings.h"
#include "ui/gfx.h"
#include "ui/theme_file.h"
#include "ui/view.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace moonsr::app {

// Command-line options for automated tests. None of them is persisted.
struct TestOptions {
    bool active = false;             // any --test-* option present
    std::wstring source;             // --test-source: select the window whose title contains this
    int frameGen = 0;                // --test-fg 1..20 (0 = not given; 1 = off)
    int frameGenMode = -1;           // --test-fg-mode fixed|auto (0 / 1)
    int frameGenTarget = 0;          // --test-fg-target N (turns frame generation on in Auto mode)
    std::wstring focus;              // --test-focus fgmode|fgmultiplier|fgtarget: keyboard focus on that control at start-up
    int upscaling = -1;              // --test-upscaling on|off
    bool autostart = false;          // --test-autostart
    int seconds = 0;                 // --test-seconds
    std::wstring statusFile;         // --test-status-file
    std::wstring theme;              // --test-theme light|dark|retro|<imported theme key>: overrides the setting for this run
    std::wstring importTheme;        // --test-import-theme <path>: import a theme file (kept under %LOCALAPPDATA%, override it for tests)
    std::wstring themeSelfTest;      // --test-theme-selftest <report file>: run the theme parser / catalog tests and exit
    int dpi = 0;                     // --test-dpi
    std::wstring page;               // --test-page home|upscaling|framegen|neural|loadorder|settings (diagnostics is an alias)
    bool openDropdown = false;       // --test-open-dropdown
    int clientWidth = 0;             // --test-size W,H (client size in DIPs)
    int clientHeight = 0;
    bool noMica = false;             // --test-no-mica
    int hoverX = -1, hoverY = -1;    // --test-hover x,y (client DIPs): pretend the mouse is there
    bool press = false;              // --test-press: also hold the left button down there
    std::wstring keys;               // --test-keys TAB,SPACE,... : key presses posted to the window after start-up
    std::wstring inspectModel;       // --test-inspect-model <path>: open the confirmation dialog for this file
    std::wstring importModel;        // --test-import-model <path>: import without confirmation (test store only)
    std::wstring modelsDir;          // --test-models-dir <dir>: store directory for test runs (default: a folder under %TEMP%)
    std::wstring scaler;             // --test-scaler <name substring>: select the scaler whose name contains this
    std::wstring order;              // --test-order "neural,framegen,upscaling" (not persisted)
    bool drag = false;               // --test-drag x1,y1,x2,y2 (client DIPs): press at 1, move to 2 and keep holding
    int dragX1 = 0, dragY1 = 0, dragX2 = 0, dragY2 = 0;
    int closeToTray = -1;            // --test-close-to-tray 0|1: overrides the setting for this run
    int effects = -1;                // --test-effects 0|1: overrides the Visual effects setting for this run
    int scroll = -1;                 // --test-scroll N: page scroll position in DIPs after start-up
    bool wheel = false;              // --test-wheel x,y,notches (client DIPs; negative notches scroll down)
    int wheelX = 0, wheelY = 0, wheelNotches = 0;
    std::wstring themesDir;          // --test-themes-dir <dir>: imported-theme folder for this run (default %LOCALAPPDATA%)
};

TestOptions ParseTestOptions();

// Posted by a second instance to ask the running one to show itself (it may be hidden in the tray).
constexpr UINT WM_APP_RESTORE = WM_APP + 3;

class MainWindow {
public:
    static constexpr const wchar_t* kClassName = L"MoonSR.MainWindow";

    MainWindow();
    ~MainWindow();
    static bool Register(HINSTANCE instance);
    bool Create(HINSTANCE instance, int showCmd, const TestOptions& test);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Dispatch(UINT msg, WPARAM wp, LPARAM lp);

    // Construction / presentation
    void OnCreate();
    void WireHandlers();
    void ApplyTheme();
    void RefreshThemes();                 // theme cards from the catalog
    void OnThemeChosen();
    void ImportThemeFile(const std::wstring& path, ui::ThemeCatalog::ImportMode mode);
    void OnImportTheme();
    void OnExportTheme();
    void ConfirmRemoveTheme(const std::wstring& key);
    void SetThemeMessage(const std::wstring& text, ui::InfoBar::Severity severity);
    void SetupBackdrop();
    // Visual effects: the setting, forced off when Windows animation effects or transparency effects are off.
    bool EffectsWanted() const;
    void ApplyEffects(bool force);
    void ApplyDpiAndSize(const RECT* suggested);
    UINT CurrentDpi() const;
    SIZE OuterSizeForClientDips(int w, int h) const;
    void OnDpiChanged(int newDpi, const RECT* suggested);
    void ApplyWindowIcons();
    void OnClose();

    // Notification-area icon (close to tray). The icon exists only while the window is hidden to the tray.
    bool CloseToTrayEnabled() const;
    void HideToTray();
    void RestoreFromTray();
    bool AddTrayIcon();
    void RemoveTrayIcon();
    void OnTrayMessage(WPARAM wp, LPARAM lp);
    void ShowTrayMenu(POINT anchor);

    // Population
    void PopulateWindows();
    void PopulateMonitors();
    void PopulateAdapters();
    void PopulateScalers();
    void ApplySettingsToUi();
    AppSettings CaptureSettings() const;
    void SaveNow();

    // Events
    void OnTimer(UINT_PTR id);
    void OnLiveChange();
    void OnFrameGenToggled();
    void OnFrameGenMode();
    void OnFrameGenValue();   // multiplier slider or target box changed
    void UpdateFrameGenUi();  // which controls of the Frame generation card are shown; help line; refresh hint

    // Neural rendering (user-supplied model)
    ModelStore& Models();
    void RefreshModels();
    void UpdateNeuralState();
    void OnNeuralChange();
    void SubmitNeuralSettings();
    void OnAddModel();
    void OpenModelsFolder();
    void BeginInspect(const std::wstring& path);
    void ShowImportDialog(const ModelInfo& info, const std::wstring& path);
    void BeginImport(const std::wstring& path);
    void BeginVerify(size_t index);
    void ConfirmRemove(size_t index);
    void SetNeuralMessage(const std::wstring& text, ui::InfoBar::Severity sev);
    void RunModelTask(std::function<std::function<void()>()> work);
    void OnModelTasksDone();
    Vendor NeuralAdapterVendor() const;
    const AdapterInfo* StageAdapter(PipelineStage stage) const;  // null = Auto (the monitor's GPU)
    const AdapterInfo* OutputAdapter() const;                    // the GPU driving the monitor that will show the image
    const ModelInfo* SelectedModel() const;
    NeuralSettings CurrentNeuralSettings() const;
    PictureSettings CurrentPictureSettings() const;
    void UpdatePictureState();
    void OnPictureChange();

    // MoonSR temporal upscaler
    bool AlsrSelected() const;
    AlsrSettings CurrentAlsr() const;
    void UpdateAlsrUi();

    // Load order
    PipelineOrder CurrentOrder() const;
    void SetOrderUi(const PipelineOrder& order);
    void RefreshOrderUi();
    void OnOrderChanged();

    // Scaling control
    void ToggleFromButton();
    void ToggleFromHotkey();
    void StartRecording();
    void OpenRecordingsFolder();
    void OpenLogFolder();
    void BeginCountdown();
    void CancelCountdown();
    void StartScaling(HWND target);
    void StopScaling();
    HWND ForegroundTarget() const;
    HWND SelectedWindow() const;
    ScaleSettings CurrentScaleSettings() const;
    void ApplyLiveSettings();
    FrameGenConfig CurrentFrameGen() const;
    void UpdateControlStates();

    // Status
    void PollEngine();
    void ShowStats(const EngineStats& st);
    void ClearStats(const wchar_t* stateText);
    void SetMessage(const std::wstring& text, ui::InfoBar::Severity sev = ui::InfoBar::Severity::Info);
    void SetStatusMessage(const std::wstring& text, ui::InfoBar::Severity sev);  // honours the hold after a recording

    // Test hooks
    void ApplyTestOptionsEarly();
    void ApplyTestOptionsLate();
    void WriteTestStatus();

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    UINT dpi_ = 96;
    bool mica_ = false;
    bool effectsSetting_ = true;  // the Visual effects switch (persisted)
    int effectsApplied_ = -1;     // last effective state given to the view (-1 = none yet)
    bool trayIconShown_ = false;  // window hidden to the tray and the icon registered
    bool exiting_ = false;        // a real exit was requested: WM_CLOSE must not hide to the tray
    HICON trayIcon_ = nullptr;
    ui::Gfx gfx_;
    ui::View view_;
    AppUi ui_;
    TestOptions test_;

    // Data behind the combo boxes
    std::vector<WindowInfo> windows_;
    std::vector<MonitorInfo> monitors_;
    std::vector<AdapterInfo> adapters_;  // display order: hardware first, software last
    std::vector<ScalerDesc> scalers_;    // available scalers only

    AppSettings settings_;
    Engine engine_;
    bool engineActive_ = false;  // Start() succeeded and Stop() has not been called since
    bool inStart_ = false;       // re-entrancy guard around Engine::Start
    bool lastFailed_ = false;
    // Models: the test store (only in test mode), the catalogue snapshot behind the list, the selection and the
    // single background task (hashing and copying must not block the window procedure).
    std::unique_ptr<ModelStore> testStore_;
    std::vector<ModelInfo> models_;
    std::wstring selectedModelId_;
    bool nvidiaPresent_ = false;
    bool amdPresent_ = false;
    bool intelPresent_ = false;
    bool neuralWanted_ = false;  // the user's switch position; the switch shows Off while unavailable
    bool busy_ = false;
    std::thread worker_;
    std::mutex doneMutex_;
    std::vector<std::function<void()>> done_;
    int countdown_ = 0;          // seconds remaining; 0 = no countdown running
    bool hotkeyOk_ = false;
    DWORD hotkeyError_ = 0;
    bool recordHotkeyOk_ = false;
    DWORD recordHotkeyError_ = 0;
    std::unique_ptr<ui::ThemeCatalog> themes_;
    std::wstring themeKey_ = L"dark";      // selected theme: light | dark | retro | imported theme key
    bool recording_ = false;               // a diagnostic recording is in progress (or just requested)
    ULONGLONG recordingPendingUntil_ = 0;  // grace period until the engine reports recording=true
    ULONGLONG messageHoldUntil_ = 0;       // tick count until which ShowStats must not overwrite the message
    std::wstring lastRecordingDirSeen_;
    std::wstring targetTitle_;

    // Latest values for the test status file
    struct TestStatus {
        std::wstring state = L"Idle", frameGen = L"Off", scaler, engineOrder;
        bool alsrActive = false;
        double game = 0, displayed = 0, generated = 0, latencyMs = 0;
    } status_;
};

}  // namespace moonsr::app
