// The MoonSR pages (Home, Upscaling, Frame generation, Neural rendering, Smart parallax, Load order, Settings) and the
// handles to their controls.
// Pages only build the visual structure; MainWindow wires behaviour (change handlers, engine calls) to the handles.
// Threading: UI thread only.
#pragma once

#include "ui/controls.h"
#include "ui/page.h"
#include "ui/view.h"

namespace moonsr::app {

enum PageIndex { PageHome = 0, PageUpscaling, PageFrameGen, PageNeural, PageRelief, PageLoadOrder, PageSettings };

struct AppUi {
    // Home
    ui::ComboBox *window = nullptr, *monitor = nullptr;
    ui::Button* refresh = nullptr;
    ui::ToggleSwitch* useForeground = nullptr;
    ui::Button* start = nullptr;
    ui::Row* heroRow = nullptr;
    ui::Row* scalerRow = nullptr; // its description follows the selected scaler
    ui::InfoBar* message = nullptr;
    ui::Tile *tGame = nullptr, *tDisplayed = nullptr, *tGenerated = nullptr, *tFrameGen = nullptr, *tFgDelay = nullptr,
             *tLatency = nullptr, *tInput = nullptr, *tOutput = nullptr;
    ui::Details* details = nullptr;
    ui::LinkButton* diagnosticsLink = nullptr;
    // Upscaling
    ui::ToggleSwitch* upscaling = nullptr;
    ui::ComboBox *scaler = nullptr, *fit = nullptr;
    ui::Slider* sharpness = nullptr;
    ui::ToggleSwitch *vsync = nullptr, *cursor = nullptr, *counter = nullptr;
    ui::ToggleSwitch* closeToTray = nullptr;  // Settings page, "Application" section
    ui::ToggleSwitch* visualEffects = nullptr;
    ui::Row* visualEffectsRow = nullptr;
    // MoonSR temporal upscaler (Upscaling page; shown while the scaler is MoonSR)
    ui::InfoBar *alsrInfo = nullptr, *alsrNote = nullptr;
    ui::Slider* alsrHistory = nullptr;
    ui::Row* alsrHistoryRow = nullptr;
    // Frame generation
    ui::ToggleSwitch *frameGen = nullptr, *fgHud = nullptr;
    ui::ComboBox* fgMode = nullptr;      // Fixed / Auto
    ui::Slider* fgMultiplier = nullptr;  // Fixed: 2x..20x
    ui::NumberBox* fgTarget = nullptr;   // Auto: desired frames per second
    ui::InfoBar* fgTargetHelp = nullptr; // Auto: help line + display refresh hint
    ui::Row *fgMultiplierRow = nullptr, *fgTargetRow = nullptr;
    ui::Slider* fgFlow = nullptr;
    // Neural rendering
    static constexpr int kModelRows = 8;  // installed models shown (newest first)
    ui::InfoBar *neuralInfo = nullptr, *neuralDev = nullptr, *neuralNote = nullptr, *neuralMsg = nullptr;
    ui::Button* addModel = nullptr;
    ui::Button* addLocalModel = nullptr;
    ui::Row* localModelRow = nullptr;
    ui::LinkButton* openModels = nullptr;
    ui::Row* modelRow[kModelRows] = {};
    ui::Button *modelVerify[kModelRows] = {}, *modelRemove[kModelRows] = {};
    ui::Row* modelEmptyRow = nullptr;
    ui::ComboBox* activeModel = nullptr;
    ui::ComboBox *nrPreset = nullptr, *nrStyle = nullptr;
    ui::Slider* nrPasses = nullptr;
    ui::Slider *nrResolution = nullptr, *nrIntensity = nullptr, *nrStructure = nullptr, *nrTone = nullptr,
               *nrSkin = nullptr, *nrDetail = nullptr, *nrColour = nullptr;
    ui::ToggleSwitch* nrAutoSkin = nullptr;
    ui::Button* nrReset = nullptr;
    static constexpr int kPictureControls = 10;
    ui::ToggleSwitch* pictureEnabled = nullptr;
    ui::Slider* pictureControls[kPictureControls] = {};
    ui::Button* pictureReset = nullptr;
    ui::ToggleSwitch* neural = nullptr;
    ui::Row* neuralRow = nullptr;
    ui::InfoBar* neuralHomeNote = nullptr;  // Home page
    // Neural rendering: guided setup (top of the page). One pooled slot per detected GPU.
    static constexpr int kSetupSlots = 4;
    struct SetupSlot {
        ui::Row* row = nullptr;            // GPU name, vendor and family; next-step buttons
        ui::Button *primary = nullptr, *secondary = nullptr;
        ui::Row* linkRow = nullptr;        // official pages
        ui::LinkButton *link = nullptr, *link2 = nullptr;
        ui::InfoBar* status = nullptr;     // Ready / needed / not supported, with the explanation
    };
    ui::InfoBar *nrSetupIntro = nullptr, *nrStageWarn = nullptr, *nrSetupMsg = nullptr;
    ui::Row *nrSetupRow = nullptr, *nrStageRow = nullptr;
    ui::Button *nrSetupToggle = nullptr, *nrStageUse = nullptr;
    ui::LinkButton* nrStageOpen = nullptr;
    SetupSlot nrSlot[kSetupSlots];
    // Settings: Updates
    ui::LinkButton* openReleases = nullptr;
    // Smart parallax (experimental)
    ui::InfoBar *reliefDev = nullptr, *reliefNote = nullptr, *reliefModeNote = nullptr;
    ui::ToggleSwitch *relief = nullptr, *reliefSplit = nullptr;
    ui::ComboBox* reliefMode = nullptr;  // item data: 0 Shading, 1 Parallax, 2 Motion depth
    ui::Slider *reliefStrength = nullptr, *reliefParallax = nullptr, *reliefDetail = nullptr;
    ui::Row* reliefParallaxRow = nullptr;
    // Load order
    ui::InfoBar *orderInfo = nullptr, *orderNow = nullptr, *orderNote = nullptr;
    ui::OrderList* orderList = nullptr;
    ui::ComboBox* stageGpu[3] = {};  // GPU choice per stage, indexed by PipelineStage; drawn and moved by orderList
    ui::InfoBar *gpuHelp = nullptr, *gpuNote = nullptr;  // "GPU choices apply when scaling starts" while running
    ui::Button* orderReset = nullptr;
    ui::TextBlock *orderExplain = nullptr, *addonsEmpty = nullptr;
    // Settings: Themes section
    ui::ThemePicker* themePicker = nullptr;
    ui::Button *importTheme = nullptr, *exportTheme = nullptr;
    ui::InfoBar* themeMsg = nullptr;
    // Settings: recording, hotkeys
    ui::Button *record = nullptr, *openRecordings = nullptr, *openLog = nullptr;
    ui::InfoBar* hotkeyNote = nullptr;
    ui::Row *hotkeyToggleRow = nullptr, *hotkeyRecordRow = nullptr;
};

// Detail rows on the Home page, in display order.
enum DetailRow { DetGpu, DetStageGpus, DetCapture, DetScaler, DetOrder, DetRelief, DetCpu, DetDropped, DetSkipped, DetState, DetCount };

void BuildPages(ui::View& view, AppUi& ui);

}  // namespace moonsr::app
