// The MoonSR pages (Home, Upscaling, Frame generation, Neural rendering, Load order, Diagnostics) and the handles to their controls.
// Pages only build the visual structure; MainWindow wires behaviour (change handlers, engine calls) to the handles.
// Threading: UI thread only.
#pragma once

#include "ui/controls.h"
#include "ui/page.h"
#include "ui/view.h"

namespace moonsr::app {

enum PageIndex { PageHome = 0, PageUpscaling, PageFrameGen, PageNeural, PageLoadOrder, PageDiagnostics };

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
    // Moon ALSR (Upscaling page; shown while the scaler is Moon ALSR)
    ui::InfoBar *alsrInfo = nullptr, *alsrNote = nullptr;
    ui::ToggleSwitch *alsrLearn = nullptr, *alsrUseLearned = nullptr;
    ui::Slider* alsrHistory = nullptr;
    ui::Button* alsrReset = nullptr;
    ui::LinkButton* alsrProfiles = nullptr;
    ui::Row *alsrLearnRow = nullptr, *alsrUseRow = nullptr, *alsrHistoryRow = nullptr, *alsrResetRow = nullptr, *alsrFolderRow = nullptr;
    ui::TextBlock* alsrStatus = nullptr;
    // Frame generation
    ui::ToggleSwitch *frameGen = nullptr, *fgHud = nullptr, *fgLearning = nullptr;
    ui::ComboBox* fgMultiplier = nullptr;
    ui::Slider* fgFlow = nullptr;
    ui::Button* fgLearningReset = nullptr;
    ui::LinkButton* fgLearningProfiles = nullptr;
    ui::TextBlock* fgLearningStatus = nullptr;
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
    // Load order
    ui::InfoBar *orderInfo = nullptr, *orderNow = nullptr, *orderNote = nullptr;
    ui::OrderList* orderList = nullptr;
    ui::ComboBox* stageGpu[3] = {};  // GPU choice per stage, indexed by PipelineStage; drawn and moved by orderList
    ui::InfoBar *gpuHelp = nullptr, *gpuNote = nullptr;  // "GPU choices apply when scaling starts" while running
    ui::Button* orderReset = nullptr;
    ui::TextBlock *orderExplain = nullptr, *addonsEmpty = nullptr;
    // Diagnostics
    ui::Button *record = nullptr, *openRecordings = nullptr, *openLog = nullptr;
    ui::InfoBar* hotkeyNote = nullptr;
    ui::Row *hotkeyToggleRow = nullptr, *hotkeyRecordRow = nullptr;
};

// Detail rows on the Home page, in display order.
enum DetailRow { DetGpu, DetStageGpus, DetCapture, DetScaler, DetOrder, DetCpu, DetDropped, DetSkipped, DetState, DetCount };

void BuildPages(ui::View& view, AppUi& ui);

}  // namespace moonsr::app
