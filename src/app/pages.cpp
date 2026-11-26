#include "pages.h"

#include "core/version.h"

namespace moonsr::app {
namespace {

// Segoe Fluent Icons / Segoe MDL2 Assets code points.
constexpr wchar_t kIconHome = 0xE80F;
constexpr wchar_t kIconUpscale = 0xE740;
constexpr wchar_t kIconFrames = 0xE786;
constexpr wchar_t kIconDiag = 0xE713;  // Settings gear
constexpr wchar_t kIconWindow = 0xE8A7;
constexpr wchar_t kIconForeground = 0xE73F;
constexpr wchar_t kIconPlay = 0xE768;
constexpr wchar_t kIconScaler = 0xE71E;
constexpr wchar_t kIconFit = 0xE9A6;
constexpr wchar_t kIconSharp = 0xE9E9;
constexpr wchar_t kIconEffects = 0xE790;  // Color (palette)
constexpr wchar_t kIconSync = 0xE895;
constexpr wchar_t kIconCursor = 0xE7C9;
constexpr wchar_t kIconCounter = 0xE9D2;
constexpr wchar_t kIconMonitor = 0xE7F4;
constexpr wchar_t kIconMult = 0xE762;
constexpr wchar_t kIconQuality = 0xE734;
constexpr wchar_t kIconFlow = 0xE9F5;
constexpr wchar_t kIconShield = 0xEA18;
constexpr wchar_t kIconRecord = 0xE7C8;
constexpr wchar_t kIconFolder = 0xE8B7;
constexpr wchar_t kIconKeyboard = 0xE765;
constexpr wchar_t kIconLog = 0xE8A5;
constexpr wchar_t kIconInfo = 0xE946;
constexpr wchar_t kIconNeuralNav = ui::kGlyphNeuralNet;  // vector neural-network glyph (Canvas::NeuralNetGlyph)
constexpr wchar_t kIconNeural = 0xE9A1;
constexpr wchar_t kIconOrderNav = 0xE8CB;
constexpr wchar_t kIconModel = 0xE7B8;
constexpr wchar_t kIconAddFile = 0xE8E5;
constexpr wchar_t kIconDocument = 0xE8A5;
constexpr wchar_t kIconOpenLocal = 0xE8DA;
constexpr wchar_t kIconRepeat = 0xE8EE;
constexpr wchar_t kIconPersonalize = 0xE771;
constexpr wchar_t kIconIntensity = 0xE945; // LightningBolt
constexpr wchar_t kIconStructure = 0xE81E; // MapLayers
constexpr wchar_t kIconColour = 0xE790;
constexpr wchar_t kIconBrightness = 0xE706;
constexpr wchar_t kIconContrast = 0xE7A1;
constexpr wchar_t kIconShadow = 0xE708; // QuietHours
constexpr wchar_t kIconLight = 0xE793;
constexpr wchar_t kIconWarmth = 0xEA80; // Lightbulb
constexpr wchar_t kIconTint = 0xEB42; // Drop
constexpr wchar_t kIconSkinDetection = 0xE928; // Fingerprint
constexpr wchar_t kIconFace = 0xE76E; // Emoji2
constexpr wchar_t kIconPicture = 0xE8B9;
constexpr wchar_t kIconReset = 0xE7A7; // Undo
constexpr wchar_t kIconDetail = 0xE8A3; // ZoomIn
constexpr wchar_t kIconSharpening = 0xE8FE; // Scan
constexpr wchar_t kIconReliefNav = ui::kGlyphReliefLayers;  // vector stacked-layers glyph (Canvas::ReliefLayersGlyph)
constexpr wchar_t kIconAaNav = ui::kGlyphAntiAlias;          // vector stepped/smooth edge glyph (Canvas::AntiAliasGlyph)
constexpr wchar_t kIconCompare = 0xE8A9;    // ViewAll (A/B split view)

using ui::ComboBox;
using ui::Slider;
using ui::ToggleSwitch;

constexpr float kToggleW = 100.0f;

}  // namespace

void BuildPages(ui::View& view, AppUi& u) {
    // ---------------------------------------------------------------- Home
    {
        ui::Page* p = view.AddPage(L"Home", kIconHome, L"Home");
        u.window = p->Make<ComboBox>();
        u.monitor = p->Make<ComboBox>();
        u.refresh = p->Make<ui::Button>();
        u.refresh->text = L"Refresh";
        u.useForeground = p->Make<ToggleSwitch>();
        u.start = p->Make<ui::Button>();
        u.start->accent = true;
        u.start->text = L"Start scaling";
        u.start->height = 40.0f;
        u.message = p->Make<ui::InfoBar>();
        u.diagnosticsLink = p->Make<ui::LinkButton>();
        u.diagnosticsLink->text = L"Open Settings";

        p->AddFree(p->Make<ui::HeroBanner>(), -1.0f, -1.0f);

        p->AddSection(L"Source");
        ui::Card& src = p->AddCard();
        p->AddRow(src, kIconWindow, L"Game window", L"The window to capture and scale",
                  {{u.window, 340.0f}, {u.refresh, 88.0f}});
        p->AddRow(src, kIconMonitor, L"Monitor", L"Where the scaled image is shown", {{u.monitor, 340.0f}});
        p->AddRow(src, kIconForeground, L"Use the foreground window when scaling starts",
                  L"Picks whatever you are playing when the countdown ends", {{u.useForeground, kToggleW}});

        ui::Card& hero = p->AddCard();
        ui::Row& heroRow = p->AddRow(hero, kIconPlay, L"Scaling",
                                     L"Starts after a 5 second countdown. Ctrl+Alt+S starts at once.",
                                     {{u.start, 190.0f}});
        heroRow.height = 84.0f;
        heroRow.hero = true;
        u.heroRow = &heroRow;
        u.reliefSplit = p->Make<ToggleSwitch>();
        p->AddRow(hero, kIconCompare, L"A/B compare",
                  L"Left half: the raw game image. Right half: the final image.",
                  {{u.reliefSplit, kToggleW}});
        p->AddInfo(u.message);
        u.neuralHomeNote = p->Make<ui::InfoBar>();
        u.neuralHomeNote->visible = false;
        p->AddInfo(u.neuralHomeNote);

        p->AddSection(L"Status");
        auto tile = [&](const wchar_t* label) {
            ui::Tile* t = p->Make<ui::Tile>();
            t->label = label;
            return t;
        };
        u.tGame = tile(L"Game FPS (captured)");
        u.tDisplayed = tile(L"Displayed FPS");
        u.tGenerated = tile(L"Generated FPS");
        u.tFrameGen = tile(L"Frame generation");
        u.tFgDelay = tile(L"Added latency (frame gen)");
        u.tLatency = tile(L"Capture to present");
        u.tInput = tile(L"Input resolution");
        u.tOutput = tile(L"Output resolution");
        p->AddTiles({u.tGame, u.tDisplayed, u.tGenerated, u.tFrameGen, u.tFgDelay, u.tLatency, u.tInput, u.tOutput});

        p->AddSection(L"Details");
        u.details = p->AddDetails({L"GPU", L"GPU per stage", L"Capture method", L"Scaler", L"Processing order",
                                   L"Anti-aliasing", L"Smart parallax", L"CPU per frame", L"Frames dropped",
                                   L"Generated frames: scheduler late / skipped", L"State"});
        p->AddFree(u.diagnosticsLink, 0.0f, 24.0f);
    }

    // ---------------------------------------------------------------- Upscaling
    {
        ui::Page* p = view.AddPage(L"Upscaling", kIconUpscale, L"Upscaling");
        u.upscaling = p->Make<ToggleSwitch>();
        u.scaler = p->Make<ComboBox>();
        u.fit = p->Make<ComboBox>();
        u.sharpness = p->Make<Slider>();
        u.sharpness->Setup(0, 100, 1, L"%");
        u.cursor = p->Make<ToggleSwitch>();
        u.fit->AddItem(L"Fit", 0);
        u.fit->AddItem(L"Stretch", 1);
        u.fit->AddItem(L"Integer", 2);

        ui::Card& c = p->AddCard();
        p->AddRow(c, kIconUpscale, L"Upscaling",
                  L"Off shows the game at its original size (1:1)",
                  {{u.upscaling, kToggleW}});
        u.scalerRow = &p->AddRow(c, kIconScaler, L"Scaler", L"The algorithm that enlarges the image", {{u.scaler, 240.0f}});
        p->AddRow(c, kIconFit, L"Fit mode", L"How the image fills the output", {{u.fit, 240.0f}});
        p->AddRow(c, kIconSharp, L"Sharpness", L"Extra edge sharpening after scaling", {{u.sharpness, 300.0f}});

        u.alsrInfo = p->Make<ui::InfoBar>();
        u.alsrInfo->Set(ui::InfoBar::Severity::Info,
                        L"MoonSR rebuilds detail from several frames using estimated motion. It uses more GPU time than the "
                        L"other scalers.");
        u.alsrHistory = p->Make<Slider>();
        u.alsrHistory->Setup(50, 95, 5, L"%");
        u.alsrNote = p->Make<ui::InfoBar>();
        u.alsrNote->visible = false;
        p->AddInfo(u.alsrInfo);
        ui::Card& ac = p->AddCard();
        u.alsrHistoryRow = &p->AddRow(ac, kIconFlow, L"History strength", L"How strongly earlier frames are trusted",
                                      {{u.alsrHistory, 300.0f}});
        p->AddInfo(u.alsrNote);

        p->AddSection(L"Presentation");
        ui::Card& d = p->AddCard();
        p->AddRow(d, kIconCursor, L"Show cursor in scaled image", L"Draws the mouse cursor over the scaled game",
                  {{u.cursor, kToggleW}});
    }

    // ---------------------------------------------------------------- Frame generation
    {
        ui::Page* p = view.AddPage(L"Frame generation", kIconFrames, L"Frame generation");
        u.vsync = p->Make<ToggleSwitch>();
        u.counter = p->Make<ToggleSwitch>();
        u.frameGen = p->Make<ToggleSwitch>();
        u.fgMode = p->Make<ComboBox>();
        u.fgMultiplier = p->Make<Slider>();
        u.fgMultiplier->Setup(2, 20, 1, L"x");
        u.fgTarget = p->Make<ui::NumberBox>();
        u.fgTarget->Setup(30, 1000, L"fps");
        u.fgTarget->SetValue(120);
        u.fgTargetHelp = p->Make<ui::InfoBar>();
        u.fgMode->AddItem(L"Fixed", 0);
        u.fgMode->AddItem(L"Auto (target frame rate)", 1);
        u.fgMode->SetSelected(0);
        u.fgFlow = p->Make<Slider>();
        u.fgFlow->Setup(25, 100, 5, L"%");
        u.fgHud = p->Make<ToggleSwitch>();
        ui::InfoBar* note = p->Make<ui::InfoBar>();
        note->Set(ui::InfoBar::Severity::Info,
                  L"Generated frames make motion look smoother. They add latency and do not make the game more responsive.");

        ui::Card& c = p->AddCard();
        p->AddRow(c, kIconFrames, L"Frame generation", L"Inserts generated frames between the game's real frames",
                  {{u.frameGen, kToggleW}});
        p->AddRow(c, kIconFrames, L"Mode", L"Fixed uses a set multiplier. Auto aims for a frame rate you choose",
                  {{u.fgMode, 200.0f}});
        u.fgMultiplierRow = &p->AddRow(c, kIconMult, L"Multiplier", L"Displayed frames per real frame",
                                       {{u.fgMultiplier, 300.0f}});
        u.fgTargetRow = &p->AddRow(c, kIconMult, L"Target frame rate", L"Displayed frames per second to aim for",
                                   {{u.fgTarget, 160.0f}});
        p->AddRow(c, kIconFlow, L"Flow scale", L"Resolution of the motion analysis", {{u.fgFlow, 300.0f}});
        p->AddRow(c, kIconShield, L"Protect HUD and static overlays", L"Keeps fixed on-screen elements from warping",
                  {{u.fgHud, kToggleW}});
        p->AddInfo(u.fgTargetHelp);
        p->AddInfo(note);
        p->AddSection(L"Presentation");
        ui::Card& presentation = p->AddCard();
        p->AddRow(presentation, kIconSync, L"V-Sync", L"Waits for the display refresh; avoids tearing", {{u.vsync, kToggleW}});
        p->AddRow(presentation, kIconCounter, L"Show frame counter (game / displayed)",
                  L"Overlay in the top-left corner of the output", {{u.counter, kToggleW}});
    }

    // ---------------------------------------------------------------- Neural rendering
    {
        ui::Page* p = view.AddPage(L"Neural rendering", kIconNeuralNav, L"Neural rendering");
        u.neuralInfo = p->Make<ui::InfoBar>();
        u.neuralInfo->Set(ui::InfoBar::Severity::Info,
                          L"NVIDIA GeForce RTX uses an imported DLSS model; RTX 20, 30 and 40 series need the patched FP16 runtime. AMD Radeon RX 7000 and RX 9000 series use AMDNR. Intel Arc A- and B-series use the DLSS5_INTEL runtime (1 pass, far from real time).");
        u.neuralDev = p->Make<ui::InfoBar>();
        u.neuralDev->Set(ui::InfoBar::Severity::Warning,
                         L"Experimental community integration. Compatibility depends on the runtime, GPU, and driver.");
        u.neuralNote = p->Make<ui::InfoBar>();
        u.neuralNote->visible = false;
        u.neuralMsg = p->Make<ui::InfoBar>();
        u.neuralMsg->visible = false;
        u.addModel = p->Make<ui::Button>();
        u.addModel->accent = true;
        u.addModel->text = L"Add model file...";
        u.addLocalModel = p->Make<ui::Button>();
        u.addLocalModel->text = L"Review and add";
        u.openModels = p->Make<ui::LinkButton>();
        u.openModels->text = L"Open models folder";
        u.activeModel = p->Make<ComboBox>();
        u.neural = p->Make<ToggleSwitch>();
        u.nrPreset = p->Make<ComboBox>();
        u.nrPreset->AddItem(L"Model default", 0);
        u.nrPreset->AddItem(L"Original (A)", 1);
        u.nrPreset->AddItem(L"Fast (B)", 2);
        u.nrStyle = p->Make<ComboBox>();
        u.nrStyle->AddItem(L"Default", 0); u.nrStyle->AddItem(L"Natural", 1); u.nrStyle->AddItem(L"Cinematic", 2);
        u.nrPasses = p->Make<Slider>();
        u.nrPasses->Setup(1, 20, 1, L"");
        u.nrResolution = p->Make<Slider>(); u.nrResolution->Setup(25, 100, 5, L"%");
        u.nrIntensity = p->Make<Slider>(); u.nrIntensity->Setup(0, 200, 5, L"%");
        u.nrStructure = p->Make<Slider>(); u.nrStructure->Setup(0, 200, 5, L"%");
        u.nrTone = p->Make<Slider>(); u.nrTone->Setup(0, 200, 5, L"%");
        u.nrSkin = p->Make<Slider>(); u.nrSkin->Setup(0, 200, 5, L"%");
        u.nrDetail = p->Make<Slider>(); u.nrDetail->Setup(0, 400, 5, L"%");
        u.nrColour = p->Make<Slider>(); u.nrColour->Setup(0, 400, 5, L"%");
        u.nrAutoSkin = p->Make<ToggleSwitch>();
        u.nrReset = p->Make<ui::Button>(); u.nrReset->text = L"Restore defaults";
        u.pictureEnabled = p->Make<ToggleSwitch>();
        u.pictureReset = p->Make<ui::Button>(); u.pictureReset->text = L"Reset picture";
        constexpr int pictureMin[] = {-400, -200, -200, -200, -200, 0, -100, -200, -200, 0};
        constexpr int pictureMax[] = {400, 200, 200, 200, 200, 400, 200, 200, 200, 400};
        for (int i = 0; i < AppUi::kPictureControls; ++i) {
            u.pictureControls[i] = p->Make<Slider>();
            u.pictureControls[i]->Setup(pictureMin[i], pictureMax[i], 5, L"");
            u.pictureControls[i]->SetDisplayDivisor(100);
        }

        // Guided setup: status and the next step for every GPU in this PC (filled by MainWindow::UpdateSetupPanel).
        p->AddSection(L"Set up neural rendering");
        u.nrSetupIntro = p->Make<ui::InfoBar>();
        u.nrSetupIntro->Set(ui::InfoBar::Severity::Info,
                            L"Neural rendering runs on NVIDIA GeForce RTX cards, AMD Radeon RX 7000 and RX 9000 series, and "
                            L"Intel Arc A- and B-series (on Intel it is far from real time). MoonSR includes no runtime or "
                            L"model: you import your own files below.");
        p->AddInfo(u.nrSetupIntro);
        u.nrSetupToggle = p->Make<ui::Button>();
        u.nrSetupToggle->text = L"Hide details";
        u.nrSetupToggle->visible = false;
        u.nrStageUse = p->Make<ui::Button>();
        u.nrStageUse->accent = true;
        u.nrStageUse->visible = false;
        u.nrStageOpen = p->Make<ui::LinkButton>();
        u.nrStageOpen->text = L"Load order";
        ui::Card& setup = p->AddCard();
        u.nrSetupRow = &p->AddRow(setup, kIconInfo, L"Setup status", L"Checking...", {{u.nrSetupToggle, 0.0f}});
        u.nrSetupRow->gate = nullptr;
        u.nrStageRow = &p->AddRow(setup, kIconNeural, L"GPU for neural rendering", L"Auto",
                                  {{u.nrStageUse, 0.0f}, {u.nrStageOpen, 0.0f}});
        u.nrStageRow->gate = nullptr;
        u.nrStageWarn = p->Make<ui::InfoBar>();
        u.nrStageWarn->visible = false;
        p->AddInfo(u.nrStageWarn);
        u.nrSetupMsg = p->Make<ui::InfoBar>();
        u.nrSetupMsg->visible = false;
        p->AddInfo(u.nrSetupMsg);
        for (AppUi::SetupSlot& s : u.nrSlot) {
            s.primary = p->Make<ui::Button>();
            s.secondary = p->Make<ui::Button>();
            s.link = p->Make<ui::LinkButton>();
            s.link2 = p->Make<ui::LinkButton>();
            s.status = p->Make<ui::InfoBar>();
            s.primary->visible = s.secondary->visible = s.link->visible = s.link2->visible = s.status->visible = false;
            s.primary->accent = true;
            ui::Card& gpu = p->AddCard();
            s.row = &p->AddRow(gpu, kIconMonitor, L"", L"", {{s.secondary, 0.0f}, {s.primary, 0.0f}});
            s.row->gate = nullptr;
            s.row->visible = false;
            s.linkRow = &p->AddRow(gpu, kIconOpenLocal, L"Official pages", L"", {{s.link, 0.0f}, {s.link2, 0.0f}});
            s.linkRow->gate = nullptr;
            s.linkRow->visible = false;
            p->AddInfo(s.status);
        }

        p->AddSection(L"About");
        ui::Card& nrAbout = p->AddCard();
        p->AddRow(nrAbout, kIconNeuralNav, L"What neural rendering does",
                  L"Uses a neural-network model to enhance captured frames, changing details, lighting and material "
                  L"appearance. Results depend on the model and settings and may differ from the game's original look. "
                  L"Processing runs on your GPU using runtime and model files you provide.", {});
        p->AddInfo(u.neuralInfo);
        p->AddInfo(u.neuralDev);
        p->AddInfo(u.neuralNote);

        p->AddSection(L"Model");
        ui::Card& use = p->AddCard();
        u.neuralRow = &p->AddRow(use, kIconNeuralNav, L"DLSS Neural Rendering",
                                 L"Enhances frames with the selected model.",
                                 {{u.neural, kToggleW}});
        p->AddRow(use, kIconDocument, L"Active model file", L"Choose between your imported model versions", {{u.activeModel, 310.0f}});
        u.localModelRow = &p->AddRow(use, kIconOpenLocal, L"Local model file", L"Review nvngx_dlssnr.dll from the MoonSR folder",
                                    {{u.addLocalModel, 170.0f}});
        u.localModelRow->visible = false;
        p->AddRow(use, kIconAddFile, L"Add a model file", L"The file is copied and inspected before use", {{u.addModel, 170.0f}});
        p->AddInfo(u.neuralMsg);
        auto* nrSettingsNote = p->Make<ui::InfoBar>();
        nrSettingsNote->Set(ui::InfoBar::Severity::Info,
                            L"Switch back to MoonSR to edit settings while scaling. Changes to the model, passes, or resolution may briefly pause the image.");
        p->AddInfo(nrSettingsNote);
        p->AddSection(L"Model and performance");
        ui::Card& perf = p->AddCard();
        p->AddRow(perf, kIconFlow, L"Model network", L"Original (A) or Fast (B). AMDNR uses its fast network by default.", {{u.nrPreset, 220.0f}});
        p->AddRow(perf, kIconRepeat, L"Passes", L"More passes use more GPU time and memory. AMDNR runs up to 3, Intel runs 1.", {{u.nrPasses, 300.0f}});
        p->AddRow(perf, kIconFit, L"Working resolution", L"Percent of the captured image used by the model", {{u.nrResolution, 300.0f}});
        p->AddSection(L"Appearance");
        ui::Card& appearance = p->AddCard();
        p->AddRow(appearance, kIconPersonalize, L"Style", L"Select the model's visual style", {{u.nrStyle, 220.0f}});
        p->AddRow(appearance, kIconIntensity, L"Intensity", L"Strength requested from the model", {{u.nrIntensity, 300.0f}});
        p->AddRow(appearance, kIconStructure, L"Local structure", L"Controls local detail", {{u.nrStructure, 300.0f}});
        p->AddRow(appearance, kIconContrast, L"Local tone", L"Controls local lighting and tone", {{u.nrTone, 300.0f}});
        p->AddRow(appearance, kIconSkinDetection, L"Automatic skin detection", L"Apply separate skin structure settings", {{u.nrAutoSkin, kToggleW}});
        p->AddRow(appearance, kIconFace, L"Skin structure", L"Detail strength for detected skin", {{u.nrSkin, 300.0f}});
        p->AddRow(appearance, kIconDetail, L"Detail transfer", L"How much of the model's detail is applied", {{u.nrDetail, 300.0f}});
        p->AddRow(appearance, kIconColour, L"Colour transfer", L"How much of the model's colour is applied", {{u.nrColour, 300.0f}});
        p->AddRow(appearance, kIconReset, L"Restore defaults", L"Keeps the selected model file", {{u.nrReset, 160.0f}});
        p->AddSection(L"Picture adjustments");
        ui::Card& picture = p->AddCard();
        p->AddRow(picture, kIconPicture, L"Enable picture adjustments", L"Adjust the finished neural image", {{u.pictureEnabled, kToggleW}});
        constexpr struct {
            wchar_t icon;
            const wchar_t* title;
            const wchar_t* description;
        } adjustments[] = {
            {kIconBrightness, L"Exposure", L"Stops of light; +1 doubles brightness"},
            {kIconContrast, L"Contrast", L"Separate light and dark"},
            {kIconSharp, L"Midtones", L"Adjust middle tones"},
            {kIconShadow, L"Shadows", L"Lift or deepen dark areas"},
            {kIconLight, L"Highlights", L"Adjust bright areas"},
            {kIconColour, L"Saturation", L"Colour strength; 1.00 is unchanged"},
            {kIconQuality, L"Vibrance", L"Boost muted colours"},
            {kIconWarmth, L"Temperature", L"Cooler or warmer colour balance"},
            {kIconTint, L"Tint", L"Green or magenta balance"},
            {kIconSharpening, L"Sharpening", L"Sharpen the image at capture resolution"},
        };
        static_assert(std::size(adjustments) == AppUi::kPictureControls);
        for (int i = 0; i < AppUi::kPictureControls; ++i) {
            const auto& adjustment = adjustments[i];
            p->AddRow(picture, adjustment.icon, adjustment.title, adjustment.description,
                      {{u.pictureControls[i], 300.0f}});
        }
        p->AddRow(picture, kIconReset, L"Reset picture", L"Restore picture values without changing model settings", {{u.pictureReset, 160.0f}});
        p->AddSection(L"Installed models");
        ui::Card& list = p->AddCard();
        for (int i = 0; i < AppUi::kModelRows; ++i) {
            u.modelVerify[i] = p->Make<ui::Button>();
            u.modelVerify[i]->text = L"Verify";
            u.modelRemove[i] = p->Make<ui::Button>();
            u.modelRemove[i]->text = L"Remove";
            u.modelVerify[i]->visible = u.modelRemove[i]->visible = false;
            ui::Row& r = p->AddRow(list, kIconModel, L"", L"", {{u.modelVerify[i], 88.0f}, {u.modelRemove[i], 88.0f}});
            r.gate = nullptr;
            r.visible = false;
            u.modelRow[i] = &r;
        }
        ui::Row& empty = p->AddRow(list, kIconModel, L"No models added yet",
                                   L"Use \"Add model file...\" above to add your own copy.", {});
        empty.gate = nullptr;
        u.modelEmptyRow = &empty;
        ui::Card& folder = p->AddCard();
        p->AddRow(folder, kIconFolder, L"Models folder", L"Where the copies you added are stored", {{u.openModels, 0.0f}});
    }

    // ---------------------------------------------------------------- Anti-aliasing (experimental)
    {
        ui::Page* p = view.AddPage(L"Anti-aliasing (experimental)", kIconAaNav, L"Anti-aliasing");
        u.aaDev = p->Make<ui::InfoBar>();
        u.aaDev->Set(ui::InfoBar::Severity::Info,
                     L"Smooths jagged edges in the captured picture. It is a spatial filter: MoonSR receives a finished "
                     L"image without depth, motion vectors or MSAA samples.");
        ui::InfoBar* about = p->Make<ui::InfoBar>();
        about->Set(ui::InfoBar::Severity::Info,
                   L"Experimental. Anti-aliasing estimates edge coverage from the picture and blends across edges. It "
                   L"cannot recover detail the game never rendered, and it softens content the game already "
                   L"anti-aliased. It is not a replacement for the game's own anti-aliasing.");
        u.aaNote = p->Make<ui::InfoBar>();
        u.aaNote->visible = false;
        u.aa = p->Make<ToggleSwitch>();
        u.aaMode = p->Make<ComboBox>();
        u.aaMode->AddItem(L"Spatial", 0);
        u.aaMode->AddItem(L"Directional (not built yet)", 1);
        u.aaMode->AddItem(L"Temporal (not built yet)", 2);
        u.aaMode->SetSelected(0);
        u.aaModeNote = p->Make<ui::InfoBar>();
        u.aaModeNote->Set(ui::InfoBar::Severity::Info,
                          L"Directional and Temporal are not built yet. Selecting them uses the spatial filter and says "
                          L"so on the Home page.");
        u.aaModeNote->visible = false;
        u.aaQuality = p->Make<ComboBox>();
        u.aaQuality->AddItem(L"Performance", 0);
        u.aaQuality->AddItem(L"Balanced", 1);
        u.aaQuality->AddItem(L"Quality", 2);
        u.aaQuality->SetSelected(1);
        u.aaStrength = p->Make<Slider>();
        u.aaStrength->Setup(0, 100, 5, L"%");
        u.aaStrength->SetValue(100);
        u.aaProtectText = p->Make<ToggleSwitch>();
        u.aaProtectHud = p->Make<ToggleSwitch>();

        p->AddInfo(u.aaDev);
        p->AddInfo(about);
        p->AddInfo(u.aaNote);
        ui::Card& c = p->AddCard();
        p->AddRow(c, kIconAaNav, L"Anti-aliasing", L"Smooth jagged edges in the finished picture", {{u.aa, kToggleW}});
        p->AddRow(c, kIconPersonalize, L"Method", L"Choose the anti-aliasing method", {{u.aaMode, 240.0f}});
        p->AddInfo(u.aaModeNote);
        ui::Card& look = p->AddCard();
        p->AddRow(look, kIconIntensity, L"Strength", L"How much of the filtered image is blended in",
                  {{u.aaStrength, 300.0f}});
        p->AddRow(look, kIconQuality, L"Quality", L"Performance searches 8 texels along an edge, Balanced 12, Quality 16",
                  {{u.aaQuality, 200.0f}});
        ui::Card& protect = p->AddCard();
        p->AddRow(protect, kIconShield, L"Protect text and thin lines", L"Keeps one-pixel strokes and fine text sharp",
                  {{u.aaProtectText, kToggleW}});
        p->AddRow(protect, kIconShield, L"Protect static HUD", L"Uses the thin-stroke guard in this tier; static-overlay masking needs history",
                  {{u.aaProtectHud, kToggleW}});
    }

    // ---------------------------------------------------------------- Smart parallax (experimental)
    {
        ui::Page* p = view.AddPage(L"Smart parallax (experimental)", kIconReliefNav, L"Smart parallax");
        u.reliefDev = p->Make<ui::InfoBar>();
        u.reliefDev->Set(ui::InfoBar::Severity::Info,
                         L"Shading adds highlights and shadows to textured surfaces. Parallax also shifts surface detail "
                         L"by up to a few pixels.");
        ui::InfoBar* about = p->Make<ui::InfoBar>();
        about->Set(ui::InfoBar::Severity::Info,
                   L"Experimental. Smart parallax is designed to estimate surface relief from the captured picture and "
                   L"add subtle relief shading and a small parallax that follows camera motion. It is an approximation: "
                   L"the game's depth is not available to MoonSR.");
        u.reliefNote = p->Make<ui::InfoBar>();
        u.reliefNote->visible = false;
        u.relief = p->Make<ToggleSwitch>();
        u.reliefMode = p->Make<ComboBox>();
        u.reliefMode->AddItem(L"Shading", 0);
        u.reliefMode->AddItem(L"Parallax", 1);
        u.reliefMode->AddItem(L"Motion depth", 2);
        u.reliefMode->SetSelected(0);
        u.reliefModeNote = p->Make<ui::InfoBar>();
        u.reliefModeNote->Set(ui::InfoBar::Severity::Info,
                              L"Experimental. Motion depth reuses compatible frame-generation motion when available. "
                              L"Otherwise it uses Parallax.");
        u.reliefModeNote->visible = false;
        u.reliefStrength = p->Make<Slider>();
        u.reliefStrength->Setup(0, 100, 5, L"%");
        u.reliefStrength->SetValue(50);
        u.reliefParallax = p->Make<Slider>();
        u.reliefParallax->Setup(0, 300, 25, L" px");
        u.reliefParallax->SetDisplayDivisor(100);
        u.reliefParallax->SetValue(200);
        u.reliefDetail = p->Make<Slider>();
        u.reliefDetail->Setup(75, 400, 25, L" px");
        u.reliefDetail->SetDisplayDivisor(100);
        u.reliefDetail->SetValue(200);

        p->AddInfo(u.reliefDev);
        p->AddInfo(about);
        p->AddInfo(u.reliefNote);
        ui::Card& c = p->AddCard();
        p->AddRow(c, kIconReliefNav, L"Smart parallax", L"Adds estimated depth to textured surfaces", {{u.relief, kToggleW}});
        p->AddRow(c, kIconPersonalize, L"Mode", L"Choose Shading, Parallax, or motion-based depth and protection",
                  {{u.reliefMode, 200.0f}});
        p->AddInfo(u.reliefModeNote);
        ui::Card& look = p->AddCard();
        p->AddRow(look, kIconIntensity, L"Strength", L"How strong the relief is", {{u.reliefStrength, 300.0f}});
        u.reliefParallaxRow = &p->AddRow(look, kIconFit, L"Parallax amount", L"Largest offset on the output, in pixels",
                                         {{u.reliefParallax, 300.0f}});
        p->AddRow(look, kIconDetail, L"Detail size", L"Size of the surface detail that gets relief, in game pixels",
                  {{u.reliefDetail, 300.0f}});
    }

    // ---------------------------------------------------------------- Load order
    {
        ui::Page* p = view.AddPage(L"Load order", kIconOrderNav, L"Load order");
        u.orderInfo = p->Make<ui::InfoBar>();
        u.orderInfo->Set(ui::InfoBar::Severity::Info, L"Stages run from top to bottom. Neural motion analysis uses the original captured frames.");
        u.orderNow = p->Make<ui::InfoBar>();
        u.orderNow->visible = false;
        u.orderNote = p->Make<ui::InfoBar>();
        u.orderNote->visible = false;
        u.orderList = p->Make<ui::OrderList>();
        u.gpuNote = p->Make<ui::InfoBar>();
        u.gpuNote->Set(ui::InfoBar::Severity::Info, L"GPU choices can be changed again after scaling is stopped.");
        u.gpuNote->visible = false;
        for (ui::ComboBox*& gpu : u.stageGpu) gpu = p->Make<ComboBox>();
        u.orderReset = p->Make<ui::Button>();
        u.orderReset->text = L"Reset to default order";
        u.orderExplain = p->Make<ui::TextBlock>();
        u.addonsEmpty = p->Make<ui::TextBlock>();
        u.addonsEmpty->text = L"No addons installed. Addons will appear here and can be placed anywhere in the order.";

        u.gpuHelp = p->Make<ui::InfoBar>();
        u.gpuHelp->Set(ui::InfoBar::Severity::Info,
                       L"Choose the GPU for each stage; Auto is the GPU that drives the monitor. Capture and presentation "
                       L"use the Upscaling GPU. When two consecutive stages use different GPUs, every frame is copied "
                       L"between them. GPU choices apply when scaling starts.");
        p->AddInfo(u.orderInfo);
        p->AddInfo(u.gpuHelp);
        p->AddInfo(u.orderNow);
        p->AddInfo(u.orderNote);
        p->AddFree(u.orderList, -1.0f, -1.0f);
        for (ui::ComboBox* gpu : u.stageGpu) {
            p->AddChild(gpu);  // the list positions and draws them next to their stage
        }
        p->AddInfo(u.gpuNote);
        p->AddFree(u.orderReset, 0.0f, 32.0f);
        p->AddSection(L"What this order does");
        p->AddFree(u.orderExplain, -1.0f, -1.0f);
        p->AddSection(L"Addons");
        p->AddFree(u.addonsEmpty, -1.0f, -1.0f);
    }

    // ---------------------------------------------------------------- Settings (formerly Diagnostics)
    {
        ui::Page* p = view.AddPage(L"Settings", kIconDiag, L"Settings");
        u.record = p->Make<ui::Button>();
        u.record->text = L"Record 120 frames for diagnostics";
        u.openRecordings = p->Make<ui::Button>();
        u.openRecordings->text = L"Open recordings folder";
        u.openLog = p->Make<ui::Button>();
        u.openLog->text = L"Open log folder";
        u.hotkeyNote = p->Make<ui::InfoBar>();
        u.hotkeyNote->visible = false;
        ui::TextLabel* k1 = p->Make<ui::TextLabel>();
        k1->text = L"Ctrl+Alt+S";
        k1->keycap = true;
        ui::TextLabel* k2 = p->Make<ui::TextLabel>();
        k2->text = L"Ctrl+Alt+R";
        k2->keycap = true;
        ui::TextLabel* ver = p->Make<ui::TextLabel>();
        ver->text = MOONSR_VERSION_WSTRING;
        u.closeToTray = p->Make<ToggleSwitch>();
        u.visualEffects = p->Make<ToggleSwitch>();
        u.resetNotices = p->Make<ui::Button>();
        u.resetNotices->text = L"Restore notices";

        p->AddSection(L"Application");
        ui::Card& app = p->AddCard();
        p->AddRow(app, kIconWindow, L"Close to system tray",
                  L"Closing the window keeps MoonSR running in the notification area.", {{u.closeToTray, kToggleW}});
        u.visualEffectsRow = &p->AddRow(app, kIconEffects, L"Visual effects",
                                        L"Transparency, glow and animations. Turn off for a plain look or on slower PCs.",
                                        {{u.visualEffects, kToggleW}});
        p->AddRow(app, kIconReset, L"Restore dismissed notices",
                  L"Show closed notices and warnings again.",
                  {{u.resetNotices, 160.0f}});
        u.openReleases = p->Make<ui::LinkButton>();
        u.openReleases->text = L"Open releases page";
        ui::Row& updates = p->AddRow(app, kIconSync, L"Updates", std::wstring(L"This is MoonSR ") + MOONSR_VERSION_WSTRING,
                                     {{u.openReleases, 0.0f}});
        updates.gate = nullptr;
        ui::InfoBar* updatesHelp = p->Make<ui::InfoBar>();
        updatesHelp->Set(ui::InfoBar::Severity::Info,
                         L"MoonSR never connects to the internet. Check for new versions on GitHub and run the newer "
                         L"installer; your settings are kept.");
        p->AddInfo(updatesHelp);

        p->AddSection(L"Themes");
        u.themePicker = p->Make<ui::ThemePicker>();
        u.themeMsg = p->Make<ui::InfoBar>();
        u.themeMsg->visible = false;
        u.importTheme = p->Make<ui::Button>();
        u.importTheme->text = L"Import theme...";
        u.exportTheme = p->Make<ui::Button>();
        u.exportTheme->text = L"Export current theme";
        p->AddFree(u.themePicker, -1.0f, -1.0f);
        p->AddInfo(u.themeMsg);
        ui::Card& thm = p->AddCard();
        p->AddRow(thm, kIconFolder, L"Import a theme", L"Adds a .moontheme file; imported themes appear above.", {{u.importTheme, 0.0f}});
        p->AddRow(thm, kIconSharp, L"Export the current theme", L"Saves a .moontheme template to start your own from.", {{u.exportTheme, 0.0f}});

        p->AddSection(L"Recording");
        ui::Card& rec = p->AddCard();
        p->AddRow(rec, kIconRecord, L"Frame recording",
                  L"Saves the output and the captured frames as PNG files. Scaling must be running.", {{u.record, 0.0f}});
        p->AddRow(rec, kIconFolder, L"Recordings", L"Where diagnostic recordings are saved", {{u.openRecordings, 0.0f}});

        p->AddSection(L"Hotkeys");
        ui::Card& hk = p->AddCard();
        u.hotkeyToggleRow = &p->AddRow(hk, kIconKeyboard, L"Start or stop scaling",
                                       L"Starts on the foreground window with no countdown", {{k1, 0.0f}});
        u.hotkeyRecordRow = &p->AddRow(hk, kIconRecord, L"Record frames", L"Records 120 output frames while scaling",
                                       {{k2, 0.0f}});
        p->AddInfo(u.hotkeyNote);

        p->AddSection(L"Logs and version");
        ui::Card& lg = p->AddCard();
        p->AddRow(lg, kIconLog, L"Log files", L"moonsr.log in %LOCALAPPDATA%\\MoonSR", {{u.openLog, 0.0f}});
        p->AddRow(lg, kIconInfo, L"MoonSR Suite", L"Version", {{ver, 0.0f}});
    }
}

}  // namespace moonsr::app
