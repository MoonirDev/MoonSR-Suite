#include "main_window.h"

#include "resource.h"
#include "ui/popup.h"
#include "ui/theme_file.h"

#include "core/log.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <format>
#include <wrl/client.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

namespace moonsr::app {
namespace {

constexpr int kCountdownSeconds = 5;
constexpr UINT kPollMs = 250;
constexpr UINT WM_APP_TRAY = WM_APP + 4;  // notification-area icon callback (NOTIFYICON_VERSION_4)
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kTrayCmdOpen = 1, kTrayCmdToggle = 2, kTrayCmdExit = 3;
UINT TaskbarCreatedMessage() {
    static const UINT m = RegisterWindowMessageW(L"TaskbarCreated");
    return m;
}
constexpr UINT WM_APP_TEST_LATE = WM_APP + 1;
constexpr UINT WM_APP_MODELS_DONE = WM_APP + 2;
constexpr wchar_t kFp16RuntimeHash[] = L"cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed";

using Severity = ui::InfoBar::Severity;

const wchar_t* StateName(EngineState s) {
    switch (s) {
    case EngineState::Idle: return L"Idle";
    case EngineState::Starting: return L"Starting";
    case EngineState::Running: return L"Running";
    case EngineState::Recovering: return L"Recovering";
    case EngineState::Failed: return L"Failed";
    }
    return L"Unknown";
}

std::wstring Ellipsize(const std::wstring& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen - 3) + L"...";
}

bool EqualNoCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    return FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | LINGUISTIC_IGNORECASE, hay.c_str(),
                           static_cast<int>(hay.size()), needle.c_str(), static_cast<int>(needle.size()), nullptr, nullptr,
                           nullptr, 0) >= 0;
}

std::wstring LocalNeuralModel() {
    wchar_t path[32768];
    const DWORD len = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (len == 0 || len >= std::size(path)) return {};
    std::wstring file(path, len);
    const auto slash = file.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    file.resize(slash + 1);
    file += L"nvngx_dlssnr.dll";
    const DWORD attributes = GetFileAttributesW(file.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) ? file : std::wstring{};
}

std::wstring WindowTitle(HWND h) {
    wchar_t buf[256] = {};
    GetWindowTextW(h, buf, static_cast<int>(std::size(buf)));
    return buf;
}

// Windows 11 22H2 (build 22621) or newer: DWM system backdrops (Mica) are available.
bool SupportsMica() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW vi{sizeof(vi)};
    return fn && fn(&vi) == 0 && vi.dwBuildNumber >= 22621;
}

const wchar_t* StageToken(PipelineStage s) {
    switch (s) {
    case PipelineStage::NeuralRendering: return L"neural";
    case PipelineStage::FrameGeneration: return L"framegen";
    default: return L"upscaling";
    }
}

wchar_t StageIcon(PipelineStage s) {
    switch (s) {
    case PipelineStage::NeuralRendering: return 0xE9F3;
    case PipelineStage::FrameGeneration: return 0xE786;
    default: return 0xE740;
    }
}

std::wstring OrderToString(const PipelineOrder& o) {
    std::wstring s;
    for (PipelineStage st : o.stages) {
        if (!s.empty()) s += L',';
        s += StageToken(st);
    }
    return s;
}

// Invalid or missing text gives the default order (Engine::NormalizeOrder also guards a wrong permutation).
PipelineOrder OrderFromString(const std::wstring& text) {
    PipelineOrder o;
    o.stages.clear();
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L',', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring tok = text.substr(pos, end - pos);
        tok.erase(std::remove(tok.begin(), tok.end(), L' '), tok.end());
        for (PipelineStage st : {PipelineStage::NeuralRendering, PipelineStage::FrameGeneration, PipelineStage::Upscaling})
            if (EqualNoCase(tok, StageToken(st))) o.stages.push_back(st);
        pos = end + 1;
    }
    return Engine::NormalizeOrder(o);
}

std::wstring SignatureSummary(const ModelInfo& m) {
    switch (m.signature) {
    case SignatureStatus::Valid: return m.signer.empty() ? std::wstring(L"Signed (valid)") : L"Signed by " + m.signer;
    case SignatureStatus::Unsigned: return L"Not signed";
    case SignatureStatus::Invalid: return L"Signature invalid";
    default: return L"Signature not checked";
    }
}

std::wstring ShortDate(const std::wstring& iso) {
    std::wstring s = iso.substr(0, 16);
    if (s.size() > 10 && s[10] == L'T') s[10] = L' ';
    return s;
}

void CopyToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

// The standard Windows open-file dialog (filter: *.dll, all files). Returns false when cancelled.
bool PickModelFile(HWND owner, std::wstring& path) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf())))) {
        MSR_LOG_ERROR(L"IFileOpenDialog could not be created");
        return false;
    }
    const COMDLG_FILTERSPEC filters[] = {{L"Model files (*.dll)", L"*.dll"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, filters);
    dlg->SetFileTypeIndex(1);
    dlg->SetTitle(L"Add a model file");
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    if (FAILED(dlg->Show(owner))) return false;
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(item.GetAddressOf()))) return false;
    PWSTR name = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || !name) return false;
    path = name;
    CoTaskMemFree(name);
    return true;
}

std::wstring RecordingsRoot() {
    wchar_t base[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(base) + L"\\MoonSR\\recordings";
}

void CreateDirectoriesRecursive(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Test options
// ---------------------------------------------------------------------------------------------------------------------

TestOptions ParseTestOptions() {
    TestOptions t;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return t;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a.rfind(L"--test-", 0) != 0) continue;
        t.active = true;
        if (a == L"--test-source") t.source = next();
        else if (a == L"--test-fg") t.frameGen = std::clamp(_wtoi(next().c_str()), 1, 20);
        else if (a == L"--test-fg-mode") t.frameGenMode = next() == L"auto" ? 1 : 0;
        else if (a == L"--test-fg-target") t.frameGenTarget = std::clamp(_wtoi(next().c_str()), 1, 100000);
        else if (a == L"--test-focus") t.focus = next();
        else if (a == L"--test-upscaling") t.upscaling = next() == L"off" ? 0 : 1;
        else if (a == L"--test-autostart") t.autostart = true;
        else if (a == L"--test-seconds") t.seconds = std::max(1, _wtoi(next().c_str()));
        else if (a == L"--test-status-file") t.statusFile = next();
        else if (a == L"--test-theme") {
            t.theme = next();
            for (wchar_t& ch : t.theme) ch = static_cast<wchar_t>(towlower(ch));
        }
        else if (a == L"--test-import-theme") t.importTheme = next();
        else if (a == L"--test-theme-selftest") t.themeSelfTest = next();
        else if (a == L"--test-dpi") t.dpi = std::clamp(_wtoi(next().c_str()), 96, 384);
        else if (a == L"--test-page") t.page = next();
        else if (a == L"--test-open-dropdown") t.openDropdown = true;
        else if (a == L"--test-no-mica") t.noMica = true;
        else if (a == L"--test-keys") t.keys = next();
        else if (a == L"--test-inspect-model") t.inspectModel = next();
        else if (a == L"--test-import-model") t.importModel = next();
        else if (a == L"--test-models-dir") t.modelsDir = next();
        else if (a == L"--test-order") t.order = next();
        else if (a == L"--test-close-to-tray") t.closeToTray = _wtoi(next().c_str()) != 0 ? 1 : 0;
        else if (a == L"--test-scaler") t.scaler = next();
        else if (a == L"--test-drag") {
            const std::wstring v = next();
            if (swscanf_s(v.c_str(), L"%d,%d,%d,%d", &t.dragX1, &t.dragY1, &t.dragX2, &t.dragY2) == 4) t.drag = true;
        }
        else if (a == L"--test-press") t.press = true;
        else if (a == L"--test-hover") {
            const std::wstring v = next();
            const size_t c = v.find(L',');
            if (c != std::wstring::npos) {
                t.hoverX = _wtoi(v.substr(0, c).c_str());
                t.hoverY = _wtoi(v.substr(c + 1).c_str());
            }
        }
        else if (a == L"--test-size") {
            const std::wstring v = next();
            const size_t c = v.find_first_of(L",xX");
            if (c != std::wstring::npos) {
                t.clientWidth = _wtoi(v.substr(0, c).c_str());
                t.clientHeight = _wtoi(v.substr(c + 1).c_str());
            }
        }
    }
    LocalFree(argv);
    return t;
}

// ---------------------------------------------------------------------------------------------------------------------
// Window class and creation
// ---------------------------------------------------------------------------------------------------------------------

MainWindow::MainWindow() : view_(gfx_) {}

MainWindow::~MainWindow() {
    if (worker_.joinable()) worker_.join();
}

bool MainWindow::Register(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // the whole client area is drawn by Direct2D
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0 && ui::DropdownPopup::RegisterWindowClass(instance);
}

bool MainWindow::Create(HINSTANCE instance, int showCmd, const TestOptions& test) {
    instance_ = instance;
    test_ = test;
    settings_ = LoadSettings();
    if (test_.active) {
        // Tests never touch the user's real model store.
        std::wstring dir = test_.modelsDir;
        if (dir.empty()) {
            wchar_t tmp[MAX_PATH] = {};
            const DWORD n = GetTempPathW(MAX_PATH, tmp);
            dir = (n > 0 && n < MAX_PATH) ? std::wstring(tmp) : std::wstring(L".\\");
            dir += L"MoonSR-test-models";
        }
        testStore_ = std::make_unique<ModelStore>(dir);
        MSR_LOG_INFO(L"Test mode: model store {}", testStore_->Directory());
    }
    if (!gfx_.Init()) return false;
    BuildPages(view_, ui_);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (test_.active) {
        x = 40;
        y = 20;
    } else if (settings_.hasWindowPos) {
        const POINT probe{settings_.windowX + 60, settings_.windowY + 20};
        if (MonitorFromPoint(probe, MONITOR_DEFAULTTONULL)) {
            x = settings_.windowX;
            y = settings_.windowY;
        }
    }

    hwnd_ = CreateWindowExW(0, kClassName, L"MoonSR", WS_OVERLAPPEDWINDOW, x, y, 1100, 800, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    ApplyWindowIcons();
    ShowWindow(hwnd_, test_.active ? SW_SHOWNORMAL : showCmd);
    UpdateWindow(hwnd_);
    PostMessageW(hwnd_, WM_APP_TEST_LATE, 0, 0);
    return true;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->OnMessage(msg, wp, lp);
}

LRESULT MainWindow::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    // No exception may escape a window procedure.
    try {
        return Dispatch(msg, wp, lp);
    } catch (const std::exception& e) {
        MSR_LOG_ERROR(L"Unhandled exception in window procedure (message {:#x}): {}", msg,
                      std::wstring(e.what(), e.what() + strlen(e.what())));
    } catch (...) {
        MSR_LOG_ERROR(L"Unhandled unknown exception in window procedure (message {:#x})", msg);
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT MainWindow::Dispatch(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == TaskbarCreatedMessage() && msg != 0) {  // Explorer restarted: its notification area lost our icon
        if (trayIconShown_) AddTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        OnCreate();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        view_.OnPaint();
        return 0;
    case WM_PRINTCLIENT:
    case WM_NCPAINT:
        break;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) view_.OnSize();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        const SIZE s = OuterSizeForClientDips(static_cast<int>(ui::metric::kMinClientWidth),
                                              static_cast<int>(ui::metric::kMinClientHeight));
        mmi->ptMinTrackSize.x = s.cx;
        mmi->ptMinTrackSize.y = s.cy;
        return 0;
    }
    case WM_MOUSEMOVE:
        view_.OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd_);
        view_.OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        view_.OnMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE:
        if (test_.hoverX < 0) view_.OnMouseLeave();  // a test that fakes the mouse position keeps it
        return 0;
    case WM_MOUSEWHEEL:
        view_.OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, view_.WantsHandCursor() ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !view_.DropdownOpen()) {
            if (view_.OnEscape()) return 0;
            if (countdown_ > 0) CancelCountdown();
            return 0;
        }
        if (view_.OnKeyDown(static_cast<UINT>(wp))) return 0;
        break;
    case WM_SYSKEYDOWN:
        if ((wp == VK_UP || wp == VK_DOWN) && (lp & (1 << 29)) && view_.OnAltKey(static_cast<UINT>(wp))) return 0;
        if (wp == VK_DOWN && (lp & (1 << 29))) {  // Alt+Down opens the focused drop-down
            view_.OnKeyDown(VK_F4);
            return 0;
        }
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_TIMER:
        OnTimer(static_cast<UINT_PTR>(wp));
        return 0;
    case WM_HOTKEY:
        if (wp == IDHOTKEY_TOGGLE) ToggleFromHotkey();
        else if (wp == IDHOTKEY_RECORD) StartRecording();
        return 0;
    case WM_DPICHANGED:
        OnDpiChanged(static_cast<int>(HIWORD(wp)), reinterpret_cast<const RECT*>(lp));
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) ApplyTheme();
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    case WM_THEMECHANGED:
        ApplyTheme();
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            view_.OnWindowFocusChanged(false);
            view_.OnWindowDeactivated();
        }
        break;
    case WM_CHAR:
        if (view_.OnChar(static_cast<wchar_t>(wp))) return 0;
        break;
    case WM_CLOSE:
        if (!exiting_ && CloseToTrayEnabled()) {
            HideToTray();
            return 0;
        }
        OnClose();
        return 0;
    case WM_APP_TRAY:
        OnTrayMessage(wp, lp);
        return 0;
    case WM_APP_RESTORE:
        RestoreFromTray();
        return 0;
    case WM_ENDSESSION:
        if (wp) {
            StopScaling();
            SaveNow();
        }
        return 0;
    case WM_APP_MODELS_DONE:
        OnModelTasksDone();
        return 0;
    case WM_APP_TEST_LATE:
        ApplyTestOptionsLate();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------------------------------
// Construction and presentation
// ---------------------------------------------------------------------------------------------------------------------

UINT MainWindow::CurrentDpi() const {
    if (test_.dpi > 0) return static_cast<UINT>(test_.dpi);
    const UINT d = hwnd_ ? GetDpiForWindow(hwnd_) : 0;
    return d ? d : 96;
}

SIZE MainWindow::OuterSizeForClientDips(int w, int h) const {
    const UINT dpi = dpi_ ? dpi_ : 96;
    RECT r{0, 0, static_cast<LONG>(std::lround(w * dpi / 96.0)), static_cast<LONG>(std::lround(h * dpi / 96.0))};
    AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)), dpi);
    return SIZE{r.right - r.left, r.bottom - r.top};
}

namespace {
std::wstring ThemesDirectory() {
    const std::wstring dir = AppDataDirectory();
    return dir.empty() ? std::wstring() : dir + L"\\themes";
}

// Standard open / save dialogs for .moontheme files. Both return false when cancelled.
bool PickThemeFile(HWND owner, std::wstring& path) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf())))) return false;
    const COMDLG_FILTERSPEC filters[] = {{L"MoonSR themes (*.moontheme)", L"*.moontheme"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, filters);
    dlg->SetFileTypeIndex(1);
    dlg->SetTitle(L"Import a theme");
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    if (FAILED(dlg->Show(owner))) return false;
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(item.GetAddressOf()))) return false;
    PWSTR name = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || !name) return false;
    path = name;
    CoTaskMemFree(name);
    return true;
}

bool PickThemeSavePath(HWND owner, const std::wstring& suggested, std::wstring& path) {
    Microsoft::WRL::ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(__uuidof(FileSaveDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf())))) return false;
    const COMDLG_FILTERSPEC filters[] = {{L"MoonSR themes (*.moontheme)", L"*.moontheme"}};
    dlg->SetFileTypes(1, filters);
    dlg->SetDefaultExtension(L"moontheme");
    dlg->SetFileName(suggested.c_str());
    dlg->SetTitle(L"Export the current theme");
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    if (FAILED(dlg->Show(owner))) return false;
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(item.GetAddressOf()))) return false;
    PWSTR name = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || !name) return false;
    path = name;
    CoTaskMemFree(name);
    return true;
}
}  // namespace

void MainWindow::ApplyTheme() {
    if (!themes_) themes_ = std::make_unique<ui::ThemeCatalog>(ThemesDirectory());
    const std::wstring want = !test_.theme.empty() ? test_.theme : themeKey_;
    const ui::ThemeEntry* entry = themes_->Find(want);
    if (!entry) {
        // The selected custom theme is gone (file removed or invalid): fall back to Windows 11 Dark.
        if (want == themeKey_ && themeKey_ != L"dark") MSR_LOG_WARN(L"Theme '{}' not found; using Windows 11 Dark", themeKey_);
        if (test_.theme.empty()) themeKey_ = L"dark";
        entry = themes_->Find(L"dark");
    }
    view_.SetTheme(entry->palette);
    const ui::Palette& pal = view_.palette();
    const BOOL d = pal.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof(d));
    // Windows 11: tint the title bar to the theme surface (attributes 34 border, 35 caption, 36 caption text; ignored elsewhere).
    auto bgr = [](const D2D1_COLOR_F& c) {
        return static_cast<COLORREF>(RGB(static_cast<int>(c.r * 255.0f + 0.5f), static_cast<int>(c.g * 255.0f + 0.5f), static_cast<int>(c.b * 255.0f + 0.5f)));
    };
    const COLORREF cap = bgr(pal.opaqueBg ? pal.bg : pal.dark ? ui::Mix(pal.bgB, pal.bgA, 0.18f) : pal.bgB), txt = bgr(pal.text),
                   border = bgr(pal.opaqueBg ? pal.cardStroke : pal.bgB);
    DwmSetWindowAttribute(hwnd_, 35, &cap, sizeof(cap));
    DwmSetWindowAttribute(hwnd_, 36, &txt, sizeof(txt));
    DwmSetWindowAttribute(hwnd_, 34, &border, sizeof(border));
}

void MainWindow::RefreshThemes() {
    if (!themes_) return;
    themes_->Reload();
    std::vector<ui::ThemePicker::Card> cards;
    for (const ui::ThemeEntry& e : themes_->entries()) {
        ui::ThemePicker::Card c;
        c.key = e.key;
        c.name = e.name;
        c.desc = e.description;
        c.builtin = e.builtin;
        c.palette = e.palette;
        cards.push_back(std::move(c));
    }
    ui_.themePicker->SetCards(std::move(cards));
    const std::wstring shown = !test_.theme.empty() && themes_->Find(test_.theme) ? test_.theme : themeKey_;
    ui_.themePicker->SetSelected(shown);
}

void MainWindow::OnThemeChosen() {
    themeKey_ = ui_.themePicker->selectedKey();
    test_.theme.clear();  // an explicit choice wins over --test-theme
    ApplyTheme();
    SaveNow();
}

void MainWindow::SetThemeMessage(const std::wstring& text, Severity severity) {
    ui_.themeMsg->Set(severity, text);
    ui_.themeMsg->SetVisible(!text.empty());
}

void MainWindow::OnImportTheme() {
    if (view_.DialogOpen()) return;
    std::wstring path;
    if (!PickThemeFile(hwnd_, path)) return;
    ImportThemeFile(path, ui::ThemeCatalog::ImportMode::Ask);
}

void MainWindow::ImportThemeFile(const std::wstring& path, ui::ThemeCatalog::ImportMode mode) {
    if (!themes_) return;
    std::wstring err, key;
    switch (themes_->Import(path, mode, err, key)) {
    case ui::ThemeCatalog::ImportStatus::Invalid:
        SetThemeMessage(L"Theme not imported: " + err, Severity::Error);
        MSR_LOG_WARN(L"Theme import failed: {}", err);
        break;
    case ui::ThemeCatalog::ImportStatus::NameExists: {
        const ui::ThemeEntry* e = themes_->Find(key);
        ui::ModalDialog::Spec spec;
        spec.title = L"A theme with this name exists";
        spec.body = (e ? e->name : key) + L"\\nReplace it with the imported file, or keep both (the new one gets a number added to its name)?";
        for (size_t i = 0; (i = spec.body.find(L"\\n", i)) != std::wstring::npos;) spec.body.replace(i, 2, L"\n");
        spec.primary = L"Replace";
        spec.secondary = L"Keep both";
        spec.onPrimary = [this, path] { ImportThemeFile(path, ui::ThemeCatalog::ImportMode::Replace); };
        spec.onSecondary = [this, path] { ImportThemeFile(path, ui::ThemeCatalog::ImportMode::Rename); };
        view_.ShowDialog(std::move(spec));
        break;
    }
    case ui::ThemeCatalog::ImportStatus::Imported: {
        RefreshThemes();
        const ui::ThemeEntry* e = themes_->Find(key);
        SetThemeMessage(L"Imported " + (e ? e->name : key) + L". Select its card to use it.", Severity::Success);
        break;
    }
    }
}

void MainWindow::OnExportTheme() {
    if (!themes_ || view_.DialogOpen()) return;
    std::wstring path;
    if (!PickThemeSavePath(hwnd_, themeKey_ + L".moontheme", path)) return;
    std::wstring err;
    if (themes_->Export(themeKey_, path, err)) SetThemeMessage(L"Exported the current theme to " + path, Severity::Success);
    else SetThemeMessage(L"Export failed: " + err, Severity::Error);
}

void MainWindow::ConfirmRemoveTheme(const std::wstring& key) {
    if (!themes_ || view_.DialogOpen()) return;
    const ui::ThemeEntry* e = themes_->Find(key);
    if (!e || e->builtin) return;
    ui::ModalDialog::Spec spec;
    spec.title = L"Remove this theme?";
    spec.body = e->name + L"\nThe imported copy is deleted. Your original file is not touched.";
    spec.primary = L"Remove";
    spec.secondary = L"Cancel";
    spec.onPrimary = [this, key] {
        std::wstring err;
        const bool wasCurrent = themeKey_ == key;
        if (!themes_->Remove(key, err)) {
            SetThemeMessage(L"Could not remove the theme: " + err, Severity::Error);
            return;
        }
        if (wasCurrent) {
            themeKey_ = L"dark";
            ApplyTheme();
            SaveNow();
        }
        RefreshThemes();
        SetThemeMessage(L"Theme removed.", Severity::Success);
    };
    view_.ShowDialog(std::move(spec));
}

void MainWindow::SetupBackdrop() {
    mica_ = false;
    if (!test_.noMica && SupportsMica()) {
        const int backdrop = 2;  // DWMSBT_MAINWINDOW (Mica)
        if (SUCCEEDED(DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)))) {
            const MARGINS m{-1, -1, -1, -1};
            mica_ = SUCCEEDED(DwmExtendFrameIntoClientArea(hwnd_, &m));
        }
    }
    view_.SetMica(mica_);
    MSR_LOG_INFO(L"UI backdrop: {}", mica_ ? L"Mica" : L"solid colour");
}

void MainWindow::ApplyDpiAndSize(const RECT* suggested) {
    int cw = static_cast<int>(ui::metric::kDefaultClientWidth), ch = static_cast<int>(ui::metric::kDefaultClientHeight);
    if (settings_.windowWidth > 0 && settings_.windowHeight > 0) {
        cw = std::max<int>(static_cast<int>(ui::metric::kMinClientWidth), settings_.windowWidth);
        ch = std::max<int>(static_cast<int>(ui::metric::kMinClientHeight), settings_.windowHeight);
    }
    if (test_.clientWidth > 0 && test_.clientHeight > 0) {
        cw = test_.clientWidth;
        ch = test_.clientHeight;
    }
    if (suggested) {
        // Keep the current size in DIPs across a DPI change.
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        const float oldScale = view_.Scale();
        cw = static_cast<int>(std::lround(cr.right / oldScale));
        ch = static_cast<int>(std::lround(cr.bottom / oldScale));
    }
    dpi_ = CurrentDpi();
    view_.SetDpi(dpi_);
    const SIZE s = OuterSizeForClientDips(cw, ch);
    if (suggested) SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, s.cx, s.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    else SetWindowPos(hwnd_, nullptr, 0, 0, s.cx, s.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Title bar (small) and Alt+Tab / taskbar (big) icons at the sizes the current DPI asks for.
void MainWindow::ApplyWindowIcons() {
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    auto load = [&](int metric) {
        return static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetricsForDpi(metric, dpi),
                                             GetSystemMetricsForDpi(metric == SM_CXICON ? SM_CYICON : SM_CYSMICON, dpi),
                                             LR_DEFAULTCOLOR | LR_SHARED));
    };
    if (HICON big = load(SM_CXICON)) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    if (HICON sm = load(SM_CXSMICON)) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(sm));
}

void MainWindow::OnDpiChanged(int newDpi, const RECT* suggested) {
    ApplyWindowIcons();
    if (test_.dpi > 0) return;  // layout scale is forced for the test
    (void)newDpi;
    ApplyDpiAndSize(suggested);
}

void MainWindow::OnCreate() {
    dpi_ = CurrentDpi();
    view_.Attach(hwnd_, dpi_);
    themeKey_ = settings_.theme.empty() ? std::wstring(L"dark") : settings_.theme;
    ApplyTheme();
    SetupBackdrop();

    PopulateWindows();
    PopulateMonitors();
    PopulateAdapters();
    PopulateScalers();
    ApplySettingsToUi();
    RefreshThemes();
    ApplyTestOptionsEarly();
    RefreshModels();
    RefreshOrderUi();
    WireHandlers();
    ApplyDpiAndSize(nullptr);

    hotkeyOk_ = RegisterHotKey(hwnd_, IDHOTKEY_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S') != FALSE;
    if (!hotkeyOk_) {
        hotkeyError_ = GetLastError();
        MSR_LOG_WARN(L"RegisterHotKey(Ctrl+Alt+S) failed ({})", hotkeyError_);
    }
    recordHotkeyOk_ = RegisterHotKey(hwnd_, IDHOTKEY_RECORD, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R') != FALSE;
    if (!recordHotkeyOk_) {
        recordHotkeyError_ = GetLastError();
        MSR_LOG_WARN(L"RegisterHotKey(Ctrl+Alt+R) failed ({})", recordHotkeyError_);
    }

    ClearStats(L"Idle");
    UpdateControlStates();

    std::wstring hotkeyProblem;
    if (!hotkeyOk_) {
        hotkeyProblem = std::format(L"Hotkey Ctrl+Alt+S is unavailable (error {}); another program may be using it. "
                                    L"Use the button instead.",
                                    hotkeyError_);
        ui_.hotkeyToggleRow->desc = std::format(L"Unavailable (error {}): another program may be using it", hotkeyError_);
    }
    if (!recordHotkeyOk_) {
        const std::wstring m = std::format(L"Hotkey Ctrl+Alt+R is unavailable (error {}); another program may be using it. "
                                           L"Use the Record button instead.",
                                           recordHotkeyError_);
        hotkeyProblem = hotkeyProblem.empty() ? m : hotkeyProblem + L" " + m;
        ui_.hotkeyRecordRow->desc = std::format(L"Unavailable (error {}): another program may be using it", recordHotkeyError_);
    }
    if (!hotkeyProblem.empty()) {
        ui_.hotkeyNote->Set(Severity::Warning, hotkeyProblem);
        ui_.hotkeyNote->visible = true;
        SetMessage(hotkeyProblem, Severity::Warning);
    } else {
        SetMessage(L"Ready. Press Start, then switch to your game.");
    }
    SetTimer(hwnd_, IDT_POLL, kPollMs, nullptr);
}

void MainWindow::WireHandlers() {
    for (ui::ComboBox* gpu : ui_.stageGpu) gpu->onChange = [this] { UpdateNeuralState(); };
    ui_.monitor->onChange = [this] {
        UpdateNeuralState();
        UpdateFrameGenUi();
    };
    ui_.window->onChange = [this] { UpdateNeuralState(); };
    view_.onDefaultAction = [this] { ToggleFromButton(); };
    view_.defaultActionPage = PageHome;
    ui_.start->onChange = [this] { ToggleFromButton(); };
    ui_.refresh->onChange = [this] { PopulateWindows(); };
    ui_.record->onChange = [this] { StartRecording(); };
    ui_.themePicker->onChange = [this] { OnThemeChosen(); };
    ui_.themePicker->onRemove = [this](const std::wstring& key) { ConfirmRemoveTheme(key); };
    ui_.importTheme->onChange = [this] { OnImportTheme(); };
    ui_.exportTheme->onChange = [this] { OnExportTheme(); };
    ui_.openRecordings->onChange = [this] { OpenRecordingsFolder(); };
    ui_.openLog->onChange = [this] { OpenLogFolder(); };
    ui_.diagnosticsLink->onChange = [this] { view_.Navigate(PageSettings); };
    ui_.addModel->onChange = [this] { OnAddModel(); };
    ui_.addLocalModel->onChange = [this] {
        if (busy_ || view_.DialogOpen()) return;
        const auto path = LocalNeuralModel();
        if (!path.empty()) BeginInspect(path);
        else SetNeuralMessage(L"The local model file is unavailable.", Severity::Error);
    };
    ui_.openModels->onChange = [this] { OpenModelsFolder(); };
    ui_.alsrHistory->onChange = [this] { OnLiveChange(); };
    for (int i = 0; i < AppUi::kModelRows; ++i) {
        ui_.modelVerify[i]->onChange = [this, i] { BeginVerify(static_cast<size_t>(i)); };
        ui_.modelRemove[i]->onChange = [this, i] { ConfirmRemove(static_cast<size_t>(i)); };
    }
    ui_.activeModel->onChange = [this] {
        const long long idx = ui_.activeModel->ItemData(ui_.activeModel->Selected());
        selectedModelId_ = (idx >= 0 && static_cast<size_t>(idx) < models_.size()) ? models_[static_cast<size_t>(idx)].id : std::wstring();
        OnNeuralChange();
    };
    ui_.neural->onChange = [this] {
        neuralWanted_ = ui_.neural->checked();
        OnNeuralChange();
    };
    ui_.nrAutoSkin->onChange = [this] { OnNeuralChange(); };
    for (ui::Control* control : std::initializer_list<ui::Control*>{ui_.nrPreset, ui_.nrStyle,
             ui_.nrIntensity, ui_.nrStructure, ui_.nrTone, ui_.nrSkin, ui_.nrDetail, ui_.nrColour})
        control->onChange = [this] { SubmitNeuralSettings(); };
    ui_.nrPasses->onChange = ui_.nrResolution->onChange = [this] {
        // Let a slider drag settle before rebuilding GPU resources.
        if (engineActive_ && !SetTimer(hwnd_, IDT_NEURAL_RESOLUTION, 180, nullptr)) SubmitNeuralSettings();
    };
    ui_.pictureEnabled->onChange = [this] { OnPictureChange(); };
    for (ui::Slider* slider : ui_.pictureControls) slider->onChange = [this] { OnPictureChange(); };
    ui_.pictureReset->onChange = [this] {
        const PictureSettings defaults;
        const int values[] = {defaults.exposure, defaults.contrast, defaults.midtones, defaults.shadows,
                              defaults.highlights, defaults.saturation, defaults.vibrance, defaults.temperature,
                              defaults.tint, defaults.sharpen};
        for (int i = 0; i < AppUi::kPictureControls; ++i) ui_.pictureControls[i]->SetValue(values[i]);
        OnPictureChange();
    };
    ui_.nrReset->onChange = [this] {
        const NeuralSettings s;
        ui_.nrPreset->SetSelected(static_cast<int>(s.preset));
        ui_.nrStyle->SetSelected(static_cast<int>(s.style));
        ui_.nrPasses->SetValue(static_cast<int>(s.passes));
        ui_.nrResolution->SetValue(static_cast<int>(s.resolutionPercent));
        ui_.nrIntensity->SetValue(static_cast<int>(s.intensityPercent));
        ui_.nrStructure->SetValue(static_cast<int>(s.structurePercent));
        ui_.nrTone->SetValue(static_cast<int>(s.tonePercent));
        ui_.nrSkin->SetValue(static_cast<int>(s.skinPercent));
        ui_.nrDetail->SetValue(static_cast<int>(s.detailPercent));
        ui_.nrColour->SetValue(static_cast<int>(s.colourPercent));
        ui_.nrAutoSkin->SetChecked(s.autoMask);
        OnNeuralChange();
    };
    ui_.orderList->onChange = [this] { OnOrderChanged(); };
    ui_.orderReset->onChange = [this] {
        SetOrderUi(Engine::DefaultOrder());
        OnOrderChanged();
    };

    ui_.upscaling->onChange = [this] { OnLiveChange(); };
    ui_.scaler->onChange = [this] { OnLiveChange(); };
    ui_.fit->onChange = [this] { OnLiveChange(); };
    ui_.sharpness->onChange = [this] { OnLiveChange(); };
    ui_.vsync->onChange = [this] { OnLiveChange(); };
    ui_.counter->onChange = [this] { OnLiveChange(); };
    ui_.fgFlow->onChange = [this] { OnLiveChange(); };
    ui_.fgHud->onChange = [this] { OnLiveChange(); };
    ui_.frameGen->onChange = [this] { OnFrameGenToggled(); };
    ui_.fgMode->onChange = [this] { OnFrameGenMode(); };
    ui_.fgMultiplier->onChange = [this] { OnFrameGenValue(); };
    ui_.fgTarget->onChange = [this] { OnFrameGenValue(); };
}

// ---------------------------------------------------------------------------------------------------------------------
// Population and persistence
// ---------------------------------------------------------------------------------------------------------------------

void MainWindow::PopulateWindows() {
    const HWND previous = SelectedWindow();
    windows_.clear();
    for (auto& w : Engine::ListWindows()) {
        if (w.processId == GetCurrentProcessId()) continue;
        windows_.push_back(std::move(w));
    }

    ui_.window->Clear();
    int select = 0;
    for (size_t i = 0; i < windows_.size(); ++i) {
        const WindowInfo& w = windows_[i];
        std::wstring title = w.title.empty() ? L"(untitled)" : Ellipsize(w.title, 70);
        std::wstring text = w.exeName.empty() ? std::format(L"{}  -  ({}x{})", title, w.clientSize.width, w.clientSize.height)
                                              : std::format(L"{}  -  {} ({}x{})", title, w.exeName, w.clientSize.width,
                                                            w.clientSize.height);
        ui_.window->AddItem(text, static_cast<long long>(i));
        if (previous && w.hwnd == previous) select = static_cast<int>(i);
    }
    if (windows_.empty()) ui_.window->AddItem(L"(no capturable windows found)", -1);
    ui_.window->SetSelected(select);
}

void MainWindow::PopulateMonitors() {
    monitors_ = Engine::ListMonitors();
    ui_.monitor->Clear();
    ui_.monitor->AddItem(L"Auto (same monitor as the game)", 0);
    for (size_t i = 0; i < monitors_.size(); ++i) {
        const MonitorInfo& m = monitors_[i];
        const std::wstring& name = m.friendlyName.empty() ? m.deviceName : m.friendlyName;
        const long w = m.desktopRect.right - m.desktopRect.left;
        const long h = m.desktopRect.bottom - m.desktopRect.top;
        std::wstring text = std::format(L"{}: {} {}x{}", i + 1, name, w, h);
        if (m.refreshHz > 0.0) text += std::format(L" @ {:.2f} Hz", m.refreshHz);
        if (m.primary) text += L" (primary)";
        ui_.monitor->AddItem(text, static_cast<long long>(i + 1));
    }
    ui_.monitor->SetSelected(0);
}

void MainWindow::PopulateAdapters() {
    adapters_ = Engine::ListAdapters();
    std::stable_partition(adapters_.begin(), adapters_.end(), [](const AdapterInfo& a) { return !a.isSoftware; });

    nvidiaPresent_ = false;
    amdPresent_ = false;
    intelPresent_ = false;
    for (const AdapterInfo& a : adapters_) {
        if (a.isSoftware) continue;
        nvidiaPresent_ |= a.vendor == Vendor::Nvidia;
        amdPresent_ |= a.vendor == Vendor::Amd;
        intelPresent_ |= a.vendor == Vendor::Intel;
    }

    for (ui::ComboBox* combo : ui_.stageGpu) {
        combo->Clear();
        combo->AddItem(L"Auto", 0); // the GPU that drives the output monitor (explained on the page)
    }
    for (size_t i = 0; i < adapters_.size(); ++i) {
        const AdapterInfo& a = adapters_[i];
        std::wstring text = a.name.empty() ? L"Unknown adapter" : a.name;
        if (a.isSoftware) {
            text += L" (software)";
        } else if (a.dedicatedVideoMemory >= (256ull << 20)) {
            text += std::format(L" ({:.1f} GB)", static_cast<double>(a.dedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0));
        } else {
            text += L" (shared memory)";
        }
        for (ui::ComboBox* combo : ui_.stageGpu) combo->AddItem(text, static_cast<long long>(i + 1));
    }
    for (ui::ComboBox* combo : ui_.stageGpu) combo->SetSelected(0);
}

void MainWindow::PopulateScalers() {
    scalers_.clear();
    for (const ScalerDesc& d : Engine::ListScalers()) {
        if (d.available) scalers_.push_back(d);
    }
    ui_.scaler->Clear();
    for (size_t i = 0; i < scalers_.size(); ++i) ui_.scaler->AddItem(scalers_[i].name, static_cast<long long>(i));
    if (scalers_.empty()) ui_.scaler->AddItem(L"(none available)", -1);
    ui_.scaler->SetSelected(0);
}

void MainWindow::ApplySettingsToUi() {
    ui_.useForeground->SetChecked(settings_.useForeground);
    ui_.vsync->SetChecked(settings_.vsync);
    ui_.cursor->SetChecked(settings_.showCursor);
    ui_.closeToTray->SetChecked(settings_.closeToTray);
    ui_.counter->SetChecked(settings_.showFrameCounter);
    ui_.upscaling->SetChecked(settings_.upscaling);

    // Monitor: by device name; falls back to Auto.
    int monitorSel = 0;
    if (!settings_.monitorDevice.empty()) {
        for (size_t i = 0; i < monitors_.size(); ++i) {
            if (EqualNoCase(monitors_[i].deviceName, settings_.monitorDevice)) {
                monitorSel = static_cast<int>(i + 1);
                break;
            }
        }
    }
    ui_.monitor->SetSelected(monitorSel);

    // GPU per stage: by vendorId + deviceId + name; unknown or missing adapters fall back to Auto.
    for (int st = 0; st < StageGpuCount; ++st) {
        const GpuPref& pref = settings_.stageGpu[st];
        int adapterSel = 0;
        if (!pref.IsAuto()) {
            for (size_t i = 0; i < adapters_.size(); ++i) {
                const AdapterInfo& a = adapters_[i];
                if (a.vendorId == pref.vendorId && a.deviceId == pref.deviceId && a.name == pref.name) {
                    adapterSel = static_cast<int>(i + 1);
                    break;
                }
            }
        }
        ui_.stageGpu[st]->SetSelected(adapterSel);
    }

    // Scaler: by name; falls back to bilinear, then the first available.
    int scalerSel = 0;
    bool found = false;
    for (size_t i = 0; i < scalers_.size() && !found; ++i) {
        // The temporal scaler was saved as "Moon ALSR (experimental)" before it was renamed to MoonSR.
        const bool legacyAlsr = scalers_[i].kind == ScalerKind::MoonALSR && settings_.scalerName.rfind(L"Moon ALSR", 0) == 0;
        if (!settings_.scalerName.empty() && (settings_.scalerName == scalers_[i].name || legacyAlsr)) {
            scalerSel = static_cast<int>(i);
            found = true;
        }
    }
    for (size_t i = 0; i < scalers_.size() && !found; ++i) {
        if (scalers_[i].kind == ScalerKind::Bilinear) {
            scalerSel = static_cast<int>(i);
            found = true;
        }
    }
    ui_.scaler->SetSelected(scalerSel);
    ui_.fit->SetSelected(std::clamp(settings_.fitMode, 0, 2));
    ui_.sharpness->SetValue(settings_.sharpnessPercent);

    ui_.frameGen->SetChecked(settings_.frameGenEnabled);
    ui_.fgMode->SetSelected(std::clamp(settings_.frameGenMode, 0, 1));
    ui_.fgMultiplier->SetValue(std::clamp(settings_.frameGenLastMultiplier, 2, 20));
    ui_.fgTarget->SetValue(std::clamp(settings_.frameGenTargetFps, 30, 1000));
    UpdateFrameGenUi();
    ui_.fgFlow->SetValue(std::clamp(settings_.frameGenFlowPercent, 25, 100));
    ui_.fgHud->SetChecked(settings_.frameGenHudProtection);

    ui_.alsrHistory->SetValue(std::clamp(settings_.alsrHistoryPercent, 50, 95));
    neuralWanted_ = settings_.neuralRendering;
    selectedModelId_ = settings_.neuralModelId;
    const auto& nr = settings_.neuralSettings;
    ui_.nrPreset->SetSelected(static_cast<int>(nr.preset));
    ui_.nrStyle->SetSelected(static_cast<int>(nr.style));
    ui_.nrPasses->SetValue(static_cast<int>(nr.passes));
    ui_.nrResolution->SetValue(static_cast<int>(nr.resolutionPercent));
    ui_.nrIntensity->SetValue(static_cast<int>(nr.intensityPercent));
    ui_.nrStructure->SetValue(static_cast<int>(nr.structurePercent));
    ui_.nrTone->SetValue(static_cast<int>(nr.tonePercent));
    ui_.nrSkin->SetValue(static_cast<int>(nr.skinPercent));
    ui_.nrDetail->SetValue(static_cast<int>(nr.detailPercent));
    ui_.nrColour->SetValue(static_cast<int>(nr.colourPercent));
    ui_.nrAutoSkin->SetChecked(nr.autoMask);
    const auto& pic = settings_.pictureSettings;
    const int pictureValues[] = {pic.exposure, pic.contrast, pic.midtones, pic.shadows, pic.highlights,
                                 pic.saturation, pic.vibrance, pic.temperature, pic.tint, pic.sharpen};
    ui_.pictureEnabled->SetChecked(pic.enabled);
    for (int i = 0; i < AppUi::kPictureControls; ++i) ui_.pictureControls[i]->SetValue(pictureValues[i]);
    UpdatePictureState();
    SetOrderUi(OrderFromString(settings_.pipelineOrder));
}

AppSettings MainWindow::CaptureSettings() const {
    AppSettings s = settings_;
    s.useForeground = ui_.useForeground->checked();
    s.vsync = ui_.vsync->checked();
    s.showCursor = ui_.cursor->checked();
    s.closeToTray = ui_.closeToTray->checked();
    s.theme = themeKey_;
    s.showFrameCounter = ui_.counter->checked();
    s.upscaling = ui_.upscaling->checked();
    s.fitMode = std::max(0, ui_.fit->Selected());
    s.sharpnessPercent = ui_.sharpness->value();
    {
        const FrameGenConfig fg = CurrentFrameGen();
        s.frameGenEnabled = ui_.frameGen->checked();
        s.frameGenMode = static_cast<int>(fg.mode);
        s.frameGenMultiplier = ui_.frameGen->checked() && fg.mode == FrameGenMode::Fixed ? ui_.fgMultiplier->value() : 1;
        s.frameGenLastMultiplier = ui_.fgMultiplier->value();
        s.frameGenTargetFps = ui_.fgTarget->value();
        s.frameGenFlowPercent = ui_.fgFlow->value();
        s.frameGenHudProtection = fg.settings.hudProtection;
    }

    s.alsrHistoryPercent = ui_.alsrHistory->value();
    s.neuralRendering = neuralWanted_;
    s.neuralModelId = selectedModelId_;
    s.neuralSettings = CurrentNeuralSettings();
    s.pictureSettings = CurrentPictureSettings();
    s.pipelineOrder = OrderToString(CurrentOrder());

    s.monitorDevice.clear();
    const int m = ui_.monitor->Selected();
    if (m > 0 && static_cast<size_t>(m) <= monitors_.size()) s.monitorDevice = monitors_[static_cast<size_t>(m) - 1].deviceName;

    for (int st = 0; st < StageGpuCount; ++st) {
        s.stageGpu[st] = GpuPref{};
        const int a = ui_.stageGpu[st]->Selected();
        if (a > 0 && static_cast<size_t>(a) <= adapters_.size()) {
            const AdapterInfo& info = adapters_[static_cast<size_t>(a) - 1];
            s.stageGpu[st].vendorId = info.vendorId;
            s.stageGpu[st].deviceId = info.deviceId;
            s.stageGpu[st].name = info.name;
        }
    }

    const int sc = ui_.scaler->Selected();
    if (sc >= 0) {
        const long long idx = ui_.scaler->ItemData(sc);
        if (idx >= 0 && static_cast<size_t>(idx) < scalers_.size()) s.scalerName = scalers_[static_cast<size_t>(idx)].name;
    }

    if (!IsIconic(hwnd_) && !IsZoomed(hwnd_)) {
        RECT r{}, cr{};
        if (GetWindowRect(hwnd_, &r) && GetClientRect(hwnd_, &cr)) {
            s.hasWindowPos = true;
            s.windowX = r.left;
            s.windowY = r.top;
            s.windowWidth = static_cast<int>(std::lround(cr.right / view_.Scale()));
            s.windowHeight = static_cast<int>(std::lround(cr.bottom / view_.Scale()));
        }
    }
    return s;
}

void MainWindow::SaveNow() {
    if (test_.active) return;  // test runs never change the persisted settings
    settings_ = CaptureSettings();
    if (!SaveSettings(settings_)) MSR_LOG_WARN(L"Saving settings failed");
}

// ---------------------------------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------------------------------

void MainWindow::OnTimer(UINT_PTR id) {
    if (id == ui::View::kAnimTimerId) {
        view_.OnAnimTimer();
    } else if (id == IDT_NEURAL_RESOLUTION) {
        SubmitNeuralSettings();
    } else if (id == IDT_POLL) {
        PollEngine();
    } else if (id == IDT_TESTSTATUS) {
        WriteTestStatus();
    } else if (id == IDT_TESTEXIT) {
        KillTimer(hwnd_, IDT_TESTEXIT);
        MSR_LOG_INFO(L"Test run finished; closing");
        WriteTestStatus();
        exiting_ = true;
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    } else if (id == IDT_COUNTDOWN) {
        if (--countdown_ > 0) {
            SetMessage(std::format(L"Scaling starts in {}... switch to your game", countdown_));
            UpdateControlStates();
            return;
        }
        KillTimer(hwnd_, IDT_COUNTDOWN);
        countdown_ = 0;
        HWND target = ui_.useForeground->checked() ? ForegroundTarget() : nullptr;
        if (!target) target = SelectedWindow();
        if (!target) {
            SetMessage(L"No window to scale. Pick a window in the list, or switch to your game before the countdown ends.",
                       Severity::Warning);
            ClearStats(L"Idle");
            UpdateControlStates();
            return;
        }
        StartScaling(target);
    }
}

void MainWindow::OnLiveChange() {
    UpdateControlStates();
    ApplyLiveSettings();
}

void MainWindow::OnFrameGenToggled() {
    OnLiveChange();  // the last multiplier / target stay in their controls, so switching back on restores them
}

void MainWindow::OnFrameGenMode() {
    UpdateFrameGenUi();
    OnLiveChange();
}

void MainWindow::OnFrameGenValue() {
    UpdateFrameGenUi();
    OnLiveChange();
}

void MainWindow::UpdateFrameGenUi() {
    const bool autoMode = ui_.fgMode->Selected() == 1;
    ui_.fgMultiplierRow->visible = !autoMode;
    ui_.fgMultiplier->SetVisible(!autoMode);
    ui_.fgTargetRow->visible = autoMode;
    ui_.fgTarget->SetVisible(autoMode);

    // Refresh rate of the chosen output monitor (Auto = the game monitor, unknown here: use the primary one).
    double hz = 0.0;
    const int m = ui_.monitor->Selected();
    if (m > 0 && static_cast<size_t>(m) <= monitors_.size()) {
        hz = monitors_[static_cast<size_t>(m) - 1].refreshHz;
    } else {
        for (const MonitorInfo& mi : monitors_)
            if (mi.primary) hz = mi.refreshHz;
    }
    std::wstring help =
        L"MoonSR generates as many frames as needed to reach this frame rate. With V-sync on it is capped at your display refresh rate.";
    if (hz > 0.0) help += std::format(L" Your display runs at {:.0f} Hz.", hz);
    if (ui_.fgTarget->clamped())
        ui_.fgTargetHelp->Set(ui::InfoBar::Severity::Warning,
                              std::format(L"Enter a frame rate from 30 to 1000. {} was changed to {}.",
                                          ui_.fgTarget->lastTyped(), ui_.fgTarget->value()));
    else
        ui_.fgTargetHelp->Set(ui::InfoBar::Severity::Info, help);
    ui_.fgTargetHelp->SetVisible(autoMode);
    view_.RequestLayout();
    view_.Invalidate();
}

void MainWindow::OnClose() {
    exiting_ = true;
    RemoveTrayIcon();
    KillTimer(hwnd_, IDT_POLL);
    KillTimer(hwnd_, IDT_COUNTDOWN);
    KillTimer(hwnd_, IDT_TESTSTATUS);
    KillTimer(hwnd_, IDT_TESTEXIT);
    view_.CloseDropdown();
    countdown_ = 0;
    if (hotkeyOk_) UnregisterHotKey(hwnd_, IDHOTKEY_TOGGLE);
    if (recordHotkeyOk_) UnregisterHotKey(hwnd_, IDHOTKEY_RECORD);
    StopScaling();
    SaveNow();
    if (worker_.joinable()) worker_.join();  // a model task in flight (hashing) finishes first; its result is dropped
    DestroyWindow(hwnd_);
}

// ---------------------------------------------------------------------------------------------------------------------
// Close to system tray
// ---------------------------------------------------------------------------------------------------------------------

bool MainWindow::CloseToTrayEnabled() const {
    if (test_.closeToTray >= 0) return test_.closeToTray != 0;
    return ui_.closeToTray && ui_.closeToTray->checked();
}

bool MainWindow::AddTrayIcon() {
    if (!trayIcon_) {
        if (FAILED(LoadIconMetric(instance_, MAKEINTRESOURCEW(IDI_APPICON), LIM_SMALL, &trayIcon_))) trayIcon_ = nullptr;
    }
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = trayIcon_;
    wcscpy_s(nid.szTip, L"MoonSR");
    Shell_NotifyIconW(NIM_DELETE, &nid);  // drop a stale entry first (harmless when absent)
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        MSR_LOG_WARN(L"Shell_NotifyIcon(NIM_ADD) failed ({})", GetLastError());
        return false;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    return true;
}

void MainWindow::RemoveTrayIcon() {
    if (trayIconShown_) {
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayIconShown_ = false;
    }
    if (trayIcon_) {
        DestroyIcon(trayIcon_);
        trayIcon_ = nullptr;
    }
}

void MainWindow::HideToTray() {
    view_.CloseDropdown();
    SaveNow();  // the setting itself is persisted even if the process is later killed
    if (!AddTrayIcon()) {
        // Without an icon the user could never get the window back: close normally instead.
        OnClose();
        return;
    }
    trayIconShown_ = true;
    ShowWindow(hwnd_, SW_HIDE);
    MSR_LOG_INFO(L"Window hidden to the notification area");
}

void MainWindow::RestoreFromTray() {
    if (trayIconShown_) {
        RemoveTrayIcon();
        MSR_LOG_INFO(L"Window restored from the notification area");
    }
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(hwnd_);
}

void MainWindow::OnTrayMessage(WPARAM wp, LPARAM lp) {
    switch (LOWORD(lp)) {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case NIN_SELECT:
    case NIN_KEYSELECT:
        RestoreFromTray();
        break;
    case WM_CONTEXTMENU:
    case WM_RBUTTONUP: {
        static ULONGLONG lastMenu = 0;  // WM_RBUTTONUP and WM_CONTEXTMENU may both arrive for one click
        const ULONGLONG now = GetTickCount64();
        if (now - lastMenu < 300) break;
        lastMenu = now;
        ShowTrayMenu(POINT{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
        break;
    }
    default:
        break;
    }
}

void MainWindow::ShowTrayMenu(POINT anchor) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const wchar_t* toggle = engineActive_ ? L"&Stop scaling" : (countdown_ > 0 ? L"&Cancel countdown" : L"&Start scaling (countdown)");
    AppendMenuW(menu, MF_STRING, kTrayCmdOpen, L"&Open MoonSR");
    SetMenuDefaultItem(menu, static_cast<UINT>(kTrayCmdOpen), FALSE);
    AppendMenuW(menu, MF_STRING | (inStart_ ? MF_GRAYED : 0), kTrayCmdToggle, toggle);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCmdExit, L"E&xit");
    SetForegroundWindow(hwnd_);  // required so the menu closes when the user clicks elsewhere
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, anchor.x, anchor.y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd == kTrayCmdOpen) {
        RestoreFromTray();
    } else if (cmd == kTrayCmdToggle) {
        ToggleFromButton();
    } else if (cmd == kTrayCmdExit) {
        exiting_ = true;
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Scaling control
// ---------------------------------------------------------------------------------------------------------------------

void MainWindow::ToggleFromButton() {
    if (inStart_) return;
    if (engineActive_) {
        StopScaling();
    } else if (countdown_ > 0) {
        CancelCountdown();
    } else {
        BeginCountdown();
    }
}

void MainWindow::ToggleFromHotkey() {
    if (inStart_) return;
    if (engineActive_) {
        StopScaling();
        return;
    }
    if (countdown_ > 0) CancelCountdown();
    HWND target = ForegroundTarget();
    if (!target) {
        SetMessage(L"Hotkey ignored: the foreground window cannot be scaled. Focus your game and try again.",
                   Severity::Warning);
        return;
    }
    StartScaling(target);
}

void MainWindow::StartRecording() {
    if (!engineActive_ || recording_) return;
    const std::wstring root = RecordingsRoot();
    if (root.empty()) {
        SetMessage(L"Could not start recording.", Severity::Error);
        return;
    }
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::wstring dir = std::format(L"{}\\{:04}{:02}{:02}_{:02}{:02}{:02}", root, t.wYear, t.wMonth, t.wDay, t.wHour,
                                         t.wMinute, t.wSecond);
    CreateDirectoriesRecursive(root);
    if (engine_.RecordFrames(120, dir)) {
        recording_ = true;
        recordingPendingUntil_ = GetTickCount64() + 1500;
        messageHoldUntil_ = 0;
        MSR_LOG_INFO(L"Diagnostic recording started: {}", dir);
        SetMessage(L"Recording 120 frames...");
    } else {
        MSR_LOG_WARN(L"Engine refused to start a recording");
        SetMessage(L"Could not start recording.", Severity::Error);
    }
    UpdateControlStates();
}

void MainWindow::OpenRecordingsFolder() {
    const std::wstring root = RecordingsRoot();
    if (root.empty()) return;
    CreateDirectoriesRecursive(root);
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) {
        MSR_LOG_WARN(L"ShellExecute(open recordings) failed ({})", static_cast<long long>(r));
        SetMessage(L"Could not open the recordings folder.", Severity::Error);
    }
}

void MainWindow::OpenLogFolder() {
    const std::wstring dir = AppDataDirectory();
    if (dir.empty()) {
        SetMessage(L"Could not open the log folder.", Severity::Error);
        return;
    }
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) {
        MSR_LOG_WARN(L"ShellExecute(open log folder) failed ({})", static_cast<long long>(r));
        SetMessage(L"Could not open the log folder.", Severity::Error);
    }
}

void MainWindow::BeginCountdown() {
    countdown_ = kCountdownSeconds;
    SetTimer(hwnd_, IDT_COUNTDOWN, 1000, nullptr);
    SetMessage(std::format(L"Scaling starts in {}... switch to your game", countdown_));
    ClearStats(L"Starting");
    UpdateControlStates();
}

void MainWindow::CancelCountdown() {
    KillTimer(hwnd_, IDT_COUNTDOWN);
    countdown_ = 0;
    SetMessage(L"Countdown cancelled.");
    ClearStats(L"Idle");
    UpdateControlStates();
}

HWND MainWindow::ForegroundTarget() const {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == hwnd_ || fg == GetShellWindow() || fg == GetDesktopWindow()) return nullptr;
    if (!IsWindow(fg) || !IsWindowVisible(fg) || IsIconic(fg)) return nullptr;

    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, static_cast<int>(std::size(cls)));
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return nullptr;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return nullptr;
    return fg;
}

HWND MainWindow::SelectedWindow() const {
    const int sel = ui_.window->Selected();
    if (sel < 0) return nullptr;
    const long long idx = ui_.window->ItemData(sel);
    if (idx < 0 || static_cast<size_t>(idx) >= windows_.size()) return nullptr;
    HWND h = windows_[static_cast<size_t>(idx)].hwnd;
    return IsWindow(h) ? h : nullptr;
}

ScaleSettings MainWindow::CurrentScaleSettings() const {
    ScaleSettings s;
    s.enabled = ui_.upscaling->checked();
    const int sc = ui_.scaler->Selected();
    if (sc >= 0) {
        const long long idx = ui_.scaler->ItemData(sc);
        if (idx >= 0 && static_cast<size_t>(idx) < scalers_.size()) s.scaler = scalers_[static_cast<size_t>(idx)].kind;
    }
    s.fit = static_cast<FitMode>(std::clamp(ui_.fit->Selected(), 0, 2));
    s.sharpness = static_cast<float>(ui_.sharpness->value()) / 100.0f;
    return s;
}

FrameGenConfig MainWindow::CurrentFrameGen() const {
    FrameGenConfig fg;
    const bool on = ui_.frameGen->checked();
    fg.mode = ui_.fgMode->Selected() == 1 ? FrameGenMode::Auto : FrameGenMode::Fixed;
    fg.multiplier = on && fg.mode == FrameGenMode::Fixed ? static_cast<uint32_t>(std::clamp(ui_.fgMultiplier->value(), 2, 20)) : 1u;
    fg.targetFps = on && fg.mode == FrameGenMode::Auto ? static_cast<uint32_t>(std::clamp(ui_.fgTarget->value(), 30, 1000)) : 0u;
    fg.settings.quality = FrameGenQuality::Performance;
    fg.settings.flowScale = static_cast<float>(ui_.fgFlow->value()) / 100.0f;
    fg.settings.hudProtection = ui_.fgHud->checked();
    return fg;
}

void MainWindow::ApplyLiveSettings() {
    if (!engineActive_) return;
    engine_.SetScaleSettings(CurrentScaleSettings());
    engine_.SetVsync(ui_.vsync->checked());
    engine_.SetFrameCounter(ui_.counter->checked());
    engine_.SetFrameGen(CurrentFrameGen());
    engine_.SetAlsrSettings(CurrentAlsr());
    engine_.SetPictureSettings(CurrentPictureSettings());
}

void MainWindow::StartScaling(HWND target) {
    if (inStart_ || engineActive_) return;
    inStart_ = true;

    EngineConfig cfg;
    cfg.targetWindow = target;
    cfg.scale = CurrentScaleSettings();
    cfg.vsync = ui_.vsync->checked();
    cfg.frameGen = CurrentFrameGen();
    cfg.cursor = ui_.cursor->checked() ? CursorMode::Software : CursorMode::None;
    cfg.showFrameCounter = ui_.counter->checked();
    cfg.order = CurrentOrder();
    cfg.alsr = CurrentAlsr();
    cfg.neuralRendering = ui_.neural->checked() && ui_.neural->enabled;
    cfg.neuralModelId = selectedModelId_;
    cfg.neuralSettings = CurrentNeuralSettings();
    cfg.pictureSettings = CurrentPictureSettings();

    // Re-resolve the monitor handle in case the display configuration changed since the list was built.
    const int m = ui_.monitor->Selected();
    if (m > 0 && static_cast<size_t>(m) <= monitors_.size()) {
        const std::wstring device = monitors_[static_cast<size_t>(m) - 1].deviceName;
        cfg.outputMonitor = monitors_[static_cast<size_t>(m) - 1].handle;
        for (const MonitorInfo& mi : Engine::ListMonitors()) {
            if (EqualNoCase(mi.deviceName, device)) {
                cfg.outputMonitor = mi.handle;
                break;
            }
        }
    }
    for (PipelineStage st : {PipelineStage::NeuralRendering, PipelineStage::FrameGeneration, PipelineStage::Upscaling})
        if (const AdapterInfo* a = StageAdapter(st)) cfg.stageAdapters.For(st) = a->luid;

    targetTitle_ = Ellipsize(WindowTitle(target), 60);
    lastRecordingDirSeen_ = engine_.GetStats().lastRecordingDir;
    recording_ = false;
    messageHoldUntil_ = 0;
    SaveNow();
    ClearStats(L"Starting");
    SetMessage(L"Starting...");
    UpdateWindow(hwnd_);

    MSR_LOG_INFO(L"Starting scaling on window '{}'", targetTitle_);
    std::wstring error;
    const bool ok = engine_.Start(cfg, error);
    inStart_ = false;

    if (ok) {
        engineActive_ = true;
        lastFailed_ = false;
        SetMessage(targetTitle_.empty() ? L"Scaling." : std::format(L"Scaling: {}", targetTitle_));
    } else {
        engine_.Stop();
        lastFailed_ = true;
        MSR_LOG_ERROR(L"Engine start failed: {}", error);
        ClearStats(L"Failed");
        SetMessage(error.empty() ? L"Scaling could not be started." : std::format(L"Could not start scaling: {}", error),
                   Severity::Error);
    }
    UpdateControlStates();
}

void MainWindow::StopScaling() {
    KillTimer(hwnd_, IDT_NEURAL_RESOLUTION);
    if (countdown_ > 0) {
        KillTimer(hwnd_, IDT_COUNTDOWN);
        countdown_ = 0;
    }
    if (engineActive_ || engine_.IsRunning()) {
        engine_.Stop();
        MSR_LOG_INFO(L"Scaling stopped by user");
        SetMessage(L"Scaling stopped.");
    }
    engineActive_ = false;
    lastFailed_ = false;
    recording_ = false;
    messageHoldUntil_ = 0;
    ClearStats(L"Idle");
    UpdateControlStates();
}

// ---------------------------------------------------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------------------------------------------------

void MainWindow::PollEngine() {
    if (!engineActive_) return;
    const EngineStats st = engine_.GetStats();
    if (!engine_.IsRunning()) {
        // The pipeline ended on its own: window closed, device unrecoverable, etc.
        engine_.Stop();
        engineActive_ = false;
        KillTimer(hwnd_, IDT_NEURAL_RESOLUTION);
        recording_ = false;
        messageHoldUntil_ = 0;
        lastFailed_ = true;
        const bool failed = st.state == EngineState::Failed || !st.error.empty();
        std::wstring text = st.error.empty() ? L"Scaling stopped (the game window may have been closed)."
                                             : std::format(L"Scaling stopped: {}", st.error);
        MSR_LOG_WARN(L"Engine stopped by itself: {}", st.error);
        ClearStats(failed ? L"Failed" : L"Idle");
        if (st.error.empty()) lastFailed_ = false;
        SetMessage(text, failed ? Severity::Error : Severity::Warning);
        UpdateControlStates();
        return;
    }
    ShowStats(st);
}

void MainWindow::SetStatusMessage(const std::wstring& text, Severity sev) {
    if (messageHoldUntil_ != 0 && GetTickCount64() < messageHoldUntil_) return;
    messageHoldUntil_ = 0;
    SetMessage(text, sev);
}

void MainWindow::SetMessage(const std::wstring& text, Severity sev) { ui_.message->Set(sev, text); }

void MainWindow::ShowStats(const EngineStats& st) {
    auto size = [](const Size& s) { return s.Empty() ? std::wstring(L"-") : std::format(L"{} × {}", s.width, s.height); };
    auto fps = [](double v) { return v > 0.0 ? std::format(L"{:.1f}", v) : std::wstring(L"-"); };
    auto orDash = [](const std::wstring& s) { return s.empty() ? std::wstring(L"-") : s; };

    ui_.tGame->SetValue(fps(st.capturedFps), st.capturedFps > 0.0 ? L"fps" : L"");
    ui_.tDisplayed->SetValue(fps(st.presentedFps), st.presentedFps > 0.0 ? L"fps" : L"");
    ui_.tGenerated->SetValue(fps(st.generatedFps), st.generatedFps > 0.0 ? L"fps" : L"");
    ui_.tInput->SetValue(size(st.inputSize));
    ui_.tOutput->SetValue(size(st.outputSize));

    const FrameGenConfig fgReq = CurrentFrameGen();
    const uint32_t requested = fgReq.multiplier;
    const uint32_t effective = st.frameGenMultiplier;
    std::wstring fgValue, fgNote;
    if (fgReq.mode == FrameGenMode::Auto && fgReq.targetFps > 0) {
        // Auto: the ratio is measured (displayed frames per real frame) and fractional.
        const double target = st.frameGenTargetFps > 0.0 ? st.frameGenTargetFps : static_cast<double>(fgReq.targetFps);
        fgValue = std::format(L"{:.1f}X", std::max(1.0, st.frameGenRatio));
        fgNote = std::format(L"auto, {:.0f} fps target", target);
        if (st.frameGenLimited) fgNote += std::format(L", limited from {}", fgReq.targetFps);
    } else if (st.frameGenLimited && requested > 1) {
        fgValue = effective > 1 ? std::format(L"{}X", effective) : std::wstring(L"Off");
        fgNote = std::format(L"limited from {}X", requested);
    } else {
        fgValue = effective > 1 ? std::format(L"{}X", effective) : std::wstring(L"Off");
    }
    ui_.tFrameGen->SetValue(fgValue, fgNote);

    // Recording state and completion notice.
    if (!st.lastRecordingDir.empty() && st.lastRecordingDir != lastRecordingDirSeen_) {
        lastRecordingDirSeen_ = st.lastRecordingDir;
        MSR_LOG_INFO(L"Recording saved: {}", st.lastRecordingDir);
        SetMessage(std::format(L"Recording saved: {}", st.lastRecordingDir), Severity::Success);
        messageHoldUntil_ = GetTickCount64() + 10000;
    }
    const bool nowRecording = st.recording || GetTickCount64() < recordingPendingUntil_;
    if (nowRecording != recording_) {
        recording_ = nowRecording;
        UpdateControlStates();
    }
    if (st.frameGenNote.empty()) SetStatusMessage(L"Scaling is running. Press Ctrl+Alt+S to stop.", Severity::Info);
    else SetStatusMessage(st.frameGenNote, Severity::Warning);

    const bool fgOn = effective > 1;
    ui_.tFgDelay->SetValue(fgOn ? std::format(L"{:.1f}", st.frameGenDelayMs) : std::wstring(L"-"), fgOn ? L"ms" : L"");
    ui_.tLatency->SetValue(st.latencyMs > 0.0 ? std::format(L"{:.1f}", st.latencyMs) : std::wstring(L"-"),
                           st.latencyMs > 0.0 ? L"ms" : L"");
    ui_.details->SetValue(DetGpu, orDash(st.adapterName));
    ui_.details->SetValue(DetStageGpus, st.stageGpuText);
    ui_.details->SetVisible(DetStageGpus, !st.stageGpuText.empty());
    ui_.details->SetValue(DetCapture, orDash(st.captureMethod));
    ui_.details->SetValue(DetScaler, orDash(st.scalerName));
    ui_.details->SetValue(DetOrder, orDash(st.pipelineOrderText));
    ui_.orderNow->Set(Severity::Info, L"In effect now: " + (st.pipelineOrderText.empty() ? std::wstring(L"-") : st.pipelineOrderText));
    ui_.orderNow->SetVisible(true);
    ui_.orderNote->Set(Severity::Warning, st.pipelineOrderNote);
    ui_.orderNote->SetVisible(!st.pipelineOrderNote.empty());
    for (ui::InfoBar* bar : {ui_.neuralNote, ui_.neuralHomeNote}) {
        bar->Set(Severity::Warning, st.neuralRenderingNote);
        bar->SetVisible(!st.neuralRenderingNote.empty());
    }
    ui_.alsrNote->Set(Severity::Warning, st.alsrNote);
    ui_.alsrNote->SetVisible(!st.alsrNote.empty() && AlsrSelected());
    ui_.details->SetValue(DetCpu, st.cpuFrameMs > 0.0 ? std::format(L"{:.2f} ms", st.cpuFrameMs) : std::wstring(L"-"));
    ui_.details->SetValue(DetDropped, std::to_wstring(st.framesDropped));
    ui_.details->SetValue(DetSkipped, fgOn ? std::format(L"{} / {}", st.generatedLate, st.generatedSkipped) : std::wstring(L"-"));
    ui_.details->SetValue(DetState, StateName(st.state));

    status_.state = StateName(st.state);
    status_.game = st.capturedFps;
    status_.displayed = st.presentedFps;
    status_.generated = st.generatedFps;
    status_.latencyMs = st.frameGenDelayMs;
    status_.frameGen = fgNote.empty() ? fgValue : fgValue + L" (" + fgNote + L")";
    status_.scaler = st.scalerName;
    status_.engineOrder = st.pipelineOrderText;
    status_.alsrActive = st.alsrActive;
}

void MainWindow::ClearStats(const wchar_t* stateText) {
    for (ui::Tile* t : {ui_.tGame, ui_.tDisplayed, ui_.tGenerated, ui_.tFrameGen, ui_.tFgDelay, ui_.tLatency, ui_.tInput,
                        ui_.tOutput})
        t->SetValue(L"-");
    for (size_t i = 0; i < DetCount; ++i) ui_.details->SetValue(i, i == DetState ? stateText : L"-");
    ui_.details->SetVisible(DetStageGpus, false);
    for (ui::InfoBar* bar : {ui_.orderNow, ui_.orderNote, ui_.neuralNote, ui_.neuralHomeNote, ui_.alsrNote}) bar->SetVisible(false);
    status_ = TestStatus{};
    status_.state = stateText;
}

void MainWindow::UpdateControlStates() {
    const bool running = engineActive_;
    const bool counting = countdown_ > 0;
    ui_.start->SetText(running ? L"Stop scaling" : (counting ? std::format(L"Stop countdown ({})", countdown_) : L"Start scaling"));

    // Settings that cannot change while the pipeline is running.
    const bool idle = !running;
    ui_.window->SetEnabled(idle);
    ui_.refresh->SetEnabled(idle);
    ui_.useForeground->SetEnabled(idle);
    ui_.monitor->SetEnabled(idle);
    for (ui::ComboBox* gpu : ui_.stageGpu) gpu->SetEnabled(idle);
    ui_.cursor->SetEnabled(idle);
    ui_.gpuNote->SetVisible(running);

    // Dependent groups follow their master switch.
    const bool up = ui_.upscaling->checked();
    ui_.scaler->SetEnabled(up);
    ui_.fit->SetEnabled(up);
    ui_.sharpness->SetEnabled(up);
    const bool fg = ui_.frameGen->checked();
    ui_.fgMode->SetEnabled(fg);
    ui_.fgMultiplier->SetEnabled(fg);
    ui_.fgTarget->SetEnabled(fg);
    ui_.fgFlow->SetEnabled(fg);
    ui_.fgHud->SetEnabled(fg);

    ui_.heroRow->desc = running ? L"Ctrl+Alt+S stops scaling from anywhere."
                        : counting ? L"Switch to your game now. Esc cancels the countdown."
                                   : L"Starts after a 5 second countdown. Ctrl+Alt+S starts at once.";
    UpdateNeuralState();
    UpdateAlsrUi();
    ui_.record->SetText(recording_ ? L"Recording..." : L"Record 120 frames for diagnostics");
    ui_.record->SetEnabled(running && !recording_);
    view_.SetNavStatus(running ? L"Scaling" : (counting ? L"Starting" : L"Idle"), running);
    view_.Invalidate();
}

// ---------------------------------------------------------------------------------------------------------------------
// Test hooks (command-line only; nothing here is persisted)
// ---------------------------------------------------------------------------------------------------------------------

void MainWindow::ApplyTestOptionsEarly() {
    if (!test_.active) return;
    if (!test_.source.empty()) {
        ui_.useForeground->SetChecked(false);
        for (size_t i = 0; i < windows_.size(); ++i) {
            if (ContainsNoCase(windows_[i].title, test_.source)) {
                ui_.window->SetSelected(static_cast<int>(i));
                break;
            }
        }
    }
    if (test_.frameGen > 0) {
        if (test_.frameGen == 1) {
            ui_.frameGen->SetChecked(false);
        } else {
            ui_.fgMultiplier->SetValue(test_.frameGen);
            ui_.frameGen->SetChecked(true);
        }
    }
    if (test_.frameGenMode >= 0) ui_.fgMode->SetSelected(test_.frameGenMode);
    if (test_.frameGenTarget > 0) {
        ui_.fgMode->SetSelected(1);
        ui_.fgTarget->SetValue(test_.frameGenTarget);
        ui_.frameGen->SetChecked(true);
    }
    UpdateFrameGenUi();
    if (test_.upscaling >= 0) ui_.upscaling->SetChecked(test_.upscaling == 1);
    if (!test_.scaler.empty()) {
        for (size_t i = 0; i < scalers_.size(); ++i)
            if (ContainsNoCase(scalers_[i].name, test_.scaler)) {
                ui_.scaler->SetSelected(static_cast<int>(i));
                break;
            }
    }
    if (!test_.order.empty()) SetOrderUi(OrderFromString(test_.order));
    if (!test_.page.empty()) {
        static const struct { const wchar_t* name; int page; } kPages[] = {
            {L"home", PageHome}, {L"upscaling", PageUpscaling}, {L"framegen", PageFrameGen},
            {L"settings", PageSettings}, {L"diagnostics", PageSettings}, {L"neural", PageNeural},
            {L"loadorder", PageLoadOrder}};
        for (const auto& p : kPages)
            if (EqualNoCase(test_.page, p.name)) view_.Navigate(p.page);
    }
}

void MainWindow::ApplyTestOptionsLate() {
    if (!test_.active) return;
    MSR_LOG_INFO(L"Test options active");
    if (test_.autostart) {
        HWND target = SelectedWindow();
        if (target && !test_.source.empty() && ContainsNoCase(WindowTitle(target), test_.source)) {
            StartScaling(target);
        } else {
            SetMessage(L"Test: the requested source window was not found.", Severity::Error);
            MSR_LOG_ERROR(L"Test autostart: no window matching '{}'", test_.source);
        }
    }
    if (!test_.importTheme.empty()) ImportThemeFile(test_.importTheme, ui::ThemeCatalog::ImportMode::Rename);
    if (!test_.importModel.empty()) BeginImport(test_.importModel);
    if (!test_.inspectModel.empty()) BeginInspect(test_.inspectModel);
    if (test_.seconds > 0) SetTimer(hwnd_, IDT_TESTEXIT, static_cast<UINT>(test_.seconds) * 1000, nullptr);
    if (!test_.statusFile.empty()) {
        SetTimer(hwnd_, IDT_TESTSTATUS, 1000, nullptr);
        WriteTestStatus();
    }
    // Make the test window the active window so that it is drawn with the Mica backdrop and the active title bar.
    keybd_event(VK_MENU, 0, 0, 0);
    SetForegroundWindow(hwnd_);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    if (!test_.focus.empty()) {
        ui::Control* f = EqualNoCase(test_.focus, L"fgmode")         ? static_cast<ui::Control*>(ui_.fgMode)
                         : EqualNoCase(test_.focus, L"fgmultiplier") ? static_cast<ui::Control*>(ui_.fgMultiplier)
                         : EqualNoCase(test_.focus, L"fgtarget")     ? static_cast<ui::Control*>(ui_.fgTarget)
                                                                     : nullptr;
        if (f) view_.FocusControl(f, true);
    }
    if (!test_.keys.empty()) {
        static const struct { const wchar_t* name; UINT vk; } kKeys[] = {
            {L"TAB", VK_TAB}, {L"ENTER", VK_RETURN}, {L"SPACE", VK_SPACE}, {L"ESC", VK_ESCAPE}, {L"UP", VK_UP},
            {L"DOWN", VK_DOWN}, {L"LEFT", VK_LEFT}, {L"RIGHT", VK_RIGHT}, {L"HOME", VK_HOME}, {L"END", VK_END},
            {L"PGDN", VK_NEXT}, {L"PGUP", VK_PRIOR}, {L"BACK", VK_BACK}, {L"DELETE", VK_DELETE}};
        size_t pos = 0;
        while (pos <= test_.keys.size()) {
            size_t end = test_.keys.find(L',', pos);
            if (end == std::wstring::npos) end = test_.keys.size();
            const std::wstring name = test_.keys.substr(pos, end - pos);
            if (name.rfind(L"type:", 0) == 0) {  // type:120 posts one WM_CHAR per character
                for (size_t i = 5; i < name.size(); ++i) PostMessageW(hwnd_, WM_CHAR, name[i], 0);
                pos = end + 1;
                continue;
            }
            for (const auto& k : kKeys)
                if (EqualNoCase(name, k.name)) PostMessageW(hwnd_, WM_KEYDOWN, k.vk, 0);
            pos = end + 1;
        }
    }
    if (test_.openDropdown) {
        if (ui::ComboBox* c = view_.FirstComboOnPage()) view_.OpenDropdown(c);
    }
    if (test_.drag) {
        const float s = view_.Scale();
        auto px = [s](int v) { return static_cast<int>(std::lround(static_cast<float>(v) * s)); };
        SendMessageW(hwnd_, WM_MOUSEMOVE, 0, MAKELPARAM(px(test_.dragX1), px(test_.dragY1)));
        view_.OnMouseDown(px(test_.dragX1), px(test_.dragY1));
        SendMessageW(hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(px(test_.dragX2), px(test_.dragY2)));
    }
    if (test_.hoverX >= 0) {
        const int px = static_cast<int>(std::lround(test_.hoverX * view_.Scale())), py = static_cast<int>(std::lround(test_.hoverY * view_.Scale()));
        SendMessageW(hwnd_, WM_MOUSEMOVE, 0, MAKELPARAM(px, py));
        if (test_.press) view_.OnMouseDown(px, py);
    }
}

void MainWindow::WriteTestStatus() {
    if (test_.statusFile.empty()) return;
    const std::wstring text =
        std::format(L"state={}\r\ngameFps={:.1f}\r\ndisplayedFps={:.1f}\r\ngeneratedFps={:.1f}\r\nframeGen={}\r\n"
                    L"addedLatencyMs={:.1f}\r\nscaler={}\r\norder={}\r\nengineOrder={}\r\n"
                    L"alsrActive={}\r\nalsrStability={}\r\nfgQuality={}\r\nmessage={}\r\n"
                    L"fgOn={}\r\nfgMode={}\r\nfgMultiplier={}\r\nfgTargetFps={}\r\nfgRequestedMultiplier={}\r\nfgRequestedTarget={}\r\n",
                    status_.state, status_.game, status_.displayed, status_.generated, status_.frameGen, status_.latencyMs,
                    status_.scaler, OrderToString(CurrentOrder()), status_.engineOrder, status_.alsrActive ? 1 : 0,
                    ui_.alsrHistory->value(), static_cast<unsigned>(CurrentFrameGen().settings.quality),
                    ui_.message->message(), ui_.frameGen->checked() ? 1 : 0, ui_.fgMode->Selected(), ui_.fgMultiplier->value(),
                    ui_.fgTarget->value(), CurrentFrameGen().multiplier, CurrentFrameGen().targetFps);
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
    HANDLE f = CreateFileW(test_.statusFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Neural rendering: the user's own model file
// ---------------------------------------------------------------------------------------------------------------------

ModelStore& MainWindow::Models() { return testStore_ ? *testStore_ : Engine::Models(); }

const ModelInfo* MainWindow::SelectedModel() const {
    for (const ModelInfo& m : models_)
        if (m.id == selectedModelId_) return &m;
    return nullptr;
}

const AdapterInfo* MainWindow::StageAdapter(PipelineStage stage) const {
    const ui::ComboBox* combo = ui_.stageGpu[static_cast<int>(stage)];
    const int index = combo ? combo->Selected() : 0;
    if (index > 0 && static_cast<size_t>(index) <= adapters_.size()) return &adapters_[static_cast<size_t>(index) - 1];
    return nullptr;
}

const AdapterInfo* MainWindow::OutputAdapter() const {
    const MonitorInfo* mon = nullptr;
    const int m = ui_.monitor->Selected();
    if (m > 0 && static_cast<size_t>(m) <= monitors_.size()) {
        mon = &monitors_[static_cast<size_t>(m) - 1];
    } else if (const HWND game = SelectedWindow()) {
        // Auto: the monitor the game window is on.
        const HMONITOR hm = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
        for (const MonitorInfo& mi : monitors_)
            if (mi.handle == hm) mon = &mi;
    }
    if (!mon) return nullptr;
    for (const AdapterInfo& a : adapters_)
        if (LuidEqual(a.luid, mon->adapterLuid)) return &a;
    return nullptr;
}

Vendor MainWindow::NeuralAdapterVendor() const {
    // The GPU that matters is the one chosen for the Neural rendering stage; Auto means the monitor's GPU.
    if (const AdapterInfo* a = StageAdapter(PipelineStage::NeuralRendering)) return a->vendor;
    if (const AdapterInfo* a = OutputAdapter()) return a->vendor;
    const int capable = (amdPresent_ ? 1 : 0) + (nvidiaPresent_ ? 1 : 0) + (intelPresent_ ? 1 : 0);
    if (capable == 1) return amdPresent_ ? Vendor::Amd : nvidiaPresent_ ? Vendor::Nvidia : Vendor::Intel;
    return Vendor::Unknown;
}

NeuralSettings MainWindow::CurrentNeuralSettings() const {
    NeuralSettings s;
    s.preset = static_cast<uint32_t>(std::max(0, ui_.nrPreset->Selected()));
    s.style = static_cast<uint32_t>(std::max(0, ui_.nrStyle->Selected()));
    s.passes = static_cast<uint32_t>(ui_.nrPasses->value());
    s.resolutionPercent = static_cast<uint32_t>(ui_.nrResolution->value());
    s.intensityPercent = static_cast<uint32_t>(ui_.nrIntensity->value());
    s.structurePercent = static_cast<uint32_t>(ui_.nrStructure->value());
    s.tonePercent = static_cast<uint32_t>(ui_.nrTone->value());
    s.skinPercent = static_cast<uint32_t>(ui_.nrSkin->value());
    s.detailPercent = static_cast<uint32_t>(ui_.nrDetail->value());
    s.colourPercent = static_cast<uint32_t>(ui_.nrColour->value());
    s.autoMask = ui_.nrAutoSkin->checked();
    return s;
}

PictureSettings MainWindow::CurrentPictureSettings() const {
    PictureSettings s;
    s.enabled = ui_.pictureEnabled->checked();
    s.exposure = ui_.pictureControls[0]->value(); s.contrast = ui_.pictureControls[1]->value();
    s.midtones = ui_.pictureControls[2]->value(); s.shadows = ui_.pictureControls[3]->value();
    s.highlights = ui_.pictureControls[4]->value(); s.saturation = ui_.pictureControls[5]->value();
    s.vibrance = ui_.pictureControls[6]->value(); s.temperature = ui_.pictureControls[7]->value();
    s.tint = ui_.pictureControls[8]->value(); s.sharpen = ui_.pictureControls[9]->value();
    return s;
}

void MainWindow::UpdatePictureState() {
    for (ui::Slider* slider : ui_.pictureControls) slider->SetEnabled(ui_.pictureEnabled->checked());
}

void MainWindow::OnPictureChange() {
    UpdatePictureState();
    if (engineActive_) engine_.SetPictureSettings(CurrentPictureSettings());
}

void MainWindow::SetNeuralMessage(const std::wstring& text, Severity sev) {
    ui_.neuralMsg->Set(sev, text);
    ui_.neuralMsg->SetVisible(true);
}

// Rebuilds the installed-models list and the "Active model" choice from the catalogue.
void MainWindow::RefreshModels() {
    models_ = Models().List();
    for (int i = 0; i < AppUi::kModelRows; ++i) {
        ui::Row& row = *ui_.modelRow[i];
        const bool used = static_cast<size_t>(i) < models_.size();
        if (used) {
            const ModelInfo& m = models_[static_cast<size_t>(i)];
            row.title = m.displayName;
            const bool versionInName = !m.fileVersion.empty() && m.displayName.find(m.fileVersion) != std::wstring::npos;
            row.desc = versionInName ? std::format(L"{}   |   Added {}", SignatureSummary(m), ShortDate(m.importedAt))
                                     : std::format(L"{}   |   {}   |   Added {}",
                                                   m.fileVersion.empty() ? std::wstring(L"version unknown") : m.fileVersion,
                                                   SignatureSummary(m), ShortDate(m.importedAt));
            row.icon = m.signature == SignatureStatus::Valid ? 0xE7B8 : 0xE7BA;  // package / warning
        }
        row.visible = used;
        ui_.modelVerify[i]->SetVisible(used);
        ui_.modelRemove[i]->SetVisible(used);
    }
    ui_.modelEmptyRow->visible = models_.empty();

    ui_.activeModel->Clear();
    int select = -1, firstNeural = -1;
    for (size_t i = 0; i < models_.size(); ++i) {
        auto name = models_[i].displayName;
        if (EqualNoCase(models_[i].sha256, kFp16RuntimeHash)) name += L" (FP16)";
        ui_.activeModel->AddItem(name, static_cast<long long>(i));
        if (models_[i].id == selectedModelId_) select = static_cast<int>(i);
        if (firstNeural < 0 && models_[i].kind == ModelKind::DlssNeuralRendering) firstNeural = static_cast<int>(i);
    }
    if (models_.empty()) ui_.activeModel->AddItem(L"(no model added)", -1);
    if (select < 0) select = firstNeural >= 0 ? firstNeural : 0;
    ui_.activeModel->SetSelected(select);
    const long long idx = ui_.activeModel->ItemData(ui_.activeModel->Selected());
    selectedModelId_ = (idx >= 0 && static_cast<size_t>(idx) < models_.size()) ? models_[static_cast<size_t>(idx)].id : std::wstring();
    UpdateNeuralState();
    view_.RequestLayout();
}

void MainWindow::UpdateNeuralState() {
    const bool running = engineActive_;
    const Vendor vendor = NeuralAdapterVendor();
    const bool automatic = !running && StageAdapter(PipelineStage::NeuralRendering) == nullptr && vendor == Vendor::Unknown;
    const bool canUseAmd = vendor == Vendor::Amd || (automatic && amdPresent_);
    const bool canUseNvidia = vendor == Vendor::Nvidia || (automatic && nvidiaPresent_);
    const bool canUseIntel = vendor == Vendor::Intel || (automatic && intelPresent_);
    const bool amdReady = canUseAmd && Engine::AmdNeuralRuntimeAvailable();
    const bool intelReady = canUseIntel && Engine::IntelNeuralRuntimeAvailable();
    const auto* model = SelectedModel();
    const bool avail = amdReady || intelReady || (canUseNvidia && model && model->kind == ModelKind::DlssNeuralRendering);
    ui_.addModel->SetText(busy_ ? L"Working..." : L"Add model file...");
    ui_.addModel->SetEnabled(!busy_);
    ui_.localModelRow->visible = !LocalNeuralModel().empty();
    ui_.addLocalModel->SetVisible(ui_.localModelRow->visible);
    ui_.addLocalModel->SetEnabled(!busy_);
    for (int i = 0; i < AppUi::kModelRows; ++i) {
        ui_.modelVerify[i]->SetEnabled(!busy_);
        ui_.modelRemove[i]->SetEnabled(!busy_ && !running);
    }
    ui_.activeModel->SetEnabled(canUseNvidia && !models_.empty());
    ui_.nrStyle->SetEnabled(canUseNvidia || canUseIntel);
    ui_.nrTone->SetEnabled(canUseNvidia || canUseIntel);
    ui_.nrSkin->SetEnabled(ui_.nrAutoSkin->checked());
    ui_.neural->SetEnabled(avail);
    if (!avail) {
        ui_.neural->SetChecked(false);
    } else {
        ui_.neural->SetChecked(neuralWanted_);
    }
    const wchar_t* desc = L"Enhances frames with the selected model. RTX 20, 30 and 40 series require the patched FP16 model.";
    const int capableCount = (canUseAmd ? 1 : 0) + (canUseIntel ? 1 : 0) + (canUseNvidia ? 1 : 0);
    if (capableCount == 0) desc = L"Select an AMD, Intel or NVIDIA GPU for this stage on the Load order page.";
    else if (capableCount > 1 && (amdReady || intelReady))
        desc = L"Uses AMDNR on AMD, DLSS5_INTEL on Intel, or the selected model on NVIDIA. Choose the GPU for this stage on the Load order page.";
    else if (canUseAmd && !canUseNvidia && !canUseIntel)
        desc = amdReady ? L"AMDNR runtime · up to 3 passes. Original uses the full network."
                        : L"Add the AMDNR runtime DLL and model pack to the amd-nr folder.";
    else if (canUseIntel && !canUseNvidia && !canUseAmd)
        desc = intelReady ? L"Intel runtime (DLSS5_INTEL) · 1 pass · experimental and slow: about 0.2 s per frame at 960x576 on an Arc Pro B50."
                          : L"Add d5c_chain.dll, its shaders and dlssnr-logical.safetensors to the intel-nr folder.";
    else if (models_.empty()) desc = L"Add a model file first";
    else if (!avail) desc = L"Select a DLSS Neural Rendering model first";
    ui_.neuralRow->desc = desc;
    view_.Invalidate();
    if (ui_.orderList) RefreshOrderUi();
}

void MainWindow::OnNeuralChange() {
    UpdateNeuralState();
    SubmitNeuralSettings();
}

void MainWindow::SubmitNeuralSettings() {
    KillTimer(hwnd_, IDT_NEURAL_RESOLUTION);
    if (engineActive_)
        engine_.SetNeuralSettings(ui_.neural->checked(), selectedModelId_, CurrentNeuralSettings());
}

void MainWindow::OpenModelsFolder() {
    const std::wstring dir = Models().Directory();
    CreateDirectoriesRecursive(dir);
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) {
        MSR_LOG_WARN(L"ShellExecute(open models folder) failed ({})", static_cast<long long>(r));
        SetNeuralMessage(L"Could not open the models folder.", Severity::Error);
    }
}

// Runs `work` on a background thread; the lambda it returns runs on the UI thread when the work is done.
void MainWindow::RunModelTask(std::function<std::function<void()>()> work) {
    if (busy_) return;
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    UpdateNeuralState();
    try {
        worker_ = std::thread([this, w = std::move(work)]() mutable {
            std::function<void()> done;
            try {
                done = w();
            } catch (const std::exception& e) {
                MSR_LOG_ERROR(L"Model task failed: {}", std::wstring(e.what(), e.what() + strlen(e.what())));
            } catch (...) {
                MSR_LOG_ERROR(L"Model task failed (unknown exception)");
            }
            {
                std::lock_guard lock(doneMutex_);
                done_.push_back(std::move(done));
            }
            PostMessageW(hwnd_, WM_APP_MODELS_DONE, 0, 0);
        });
    } catch (const std::system_error&) {
        busy_ = false;
        UpdateNeuralState();
        SetNeuralMessage(L"Could not start the background task.", Severity::Error);
    }
}

void MainWindow::OnModelTasksDone() {
    std::vector<std::function<void()>> done;
    {
        std::lock_guard lock(doneMutex_);
        done.swap(done_);
    }
    if (worker_.joinable()) worker_.join();
    busy_ = false;
    for (auto& f : done)
        if (f) f();
    UpdateNeuralState();
}

void MainWindow::OnAddModel() {
    if (busy_ || view_.DialogOpen()) return;
    std::wstring path;
    if (!PickModelFile(hwnd_, path)) return;
    BeginInspect(path);
}

void MainWindow::BeginInspect(const std::wstring& path) {
    SetNeuralMessage(L"Reading the file...", Severity::Info);
    RunModelTask([this, path]() -> std::function<void()> {
        struct R {
            ModelInfo info;
            std::wstring err;
            bool ok = false;
        };
        auto r = std::make_shared<R>();
        r->ok = ModelStore::Inspect(path, r->info, r->err);
        return [this, r, path] {
            if (!r->ok) {
                SetNeuralMessage(L"This file cannot be added: " + r->err, Severity::Error);
            } else if (!r->info.is64Bit) {
                SetNeuralMessage(L"This is not a 64-bit library. MoonSR can only use 64-bit models.", Severity::Error);
            } else {
                ui_.neuralMsg->SetVisible(false);
                ShowImportDialog(r->info, path);
            }
        };
    });
}

void MainWindow::ShowImportDialog(const ModelInfo& m, const std::wstring& path) {
    using Tone = ui::ModalDialog::Tone;
    ui::ModalDialog::Spec spec;
    spec.title = L"Add this model?";

    std::wstring kind;
    Tone kindTone = Tone::Normal;
    switch (m.kind) {
    case ModelKind::DlssNeuralRendering: kind = L"DLSS Neural Rendering model"; break;
    case ModelKind::DlssSuperResolution: kind = L"DLSS Super Resolution model"; break;
    default:
        kind = L"Unknown (the file name is not a known model name)";
        kindTone = Tone::Warning;
        break;
    }
    if (EqualNoCase(m.sha256, kFp16RuntimeHash)) kind += L" (FP16 for RTX 20/30/40)";
    spec.lines.push_back({L"Detected as", kind, kindTone, {}});
    spec.lines.push_back({L"File version", m.fileVersion.empty() ? std::wstring(L"not stated") : m.fileVersion, Tone::Normal, {}});
    std::wstring maker = m.productName;
    if (!m.companyName.empty()) maker += (maker.empty() ? L"" : L"  /  ") + m.companyName;
    spec.lines.push_back({L"Product / company", maker.empty() ? std::wstring(L"not stated") : maker, Tone::Normal, {}});
    spec.lines.push_back({L"Original name", m.originalFileName.empty() ? std::wstring(L"not stated") : m.originalFileName, Tone::Normal, {}});
    spec.lines.push_back({L"Size", std::format(L"{:.1f} MB", static_cast<double>(m.fileSize) / (1024.0 * 1024.0)), Tone::Normal, {}});
    const std::wstring shortHash = m.sha256.size() == 64 ? m.sha256.substr(0, 12) + L"..." + m.sha256.substr(56) : m.sha256;
    spec.lines.push_back({L"SHA-256", shortHash, Tone::Secondary, m.sha256});

    std::wstring sig;
    Tone sigTone = Tone::Normal;
    switch (m.signature) {
    case SignatureStatus::Valid:
        sig = L"Signed by " + (m.signer.empty() ? std::wstring(L"(unnamed signer)") : m.signer) + L" (signature valid)";
        break;
    case SignatureStatus::Unsigned:
        sig = L"NOT SIGNED - this file's origin cannot be verified";
        sigTone = Tone::Error;
        break;
    case SignatureStatus::Invalid:
        sig = L"Signature INVALID" + (m.signer.empty() ? std::wstring() : L" (claims to be signed by " + m.signer + L")");
        sigTone = Tone::Error;
        break;
    default:
        sig = L"The signature could not be checked";
        sigTone = Tone::Error;
        break;
    }
    spec.lines.push_back({L"Signature", sig, sigTone, {}});

    const bool risky = m.signature != SignatureStatus::Valid || m.kind == ModelKind::Unknown;
    if (risky) {
        spec.body = L"Only add a file you trust: a feature that uses it will run code from it.";
        spec.bodyTone = Tone::Warning;
    } else {
        spec.body = L"MoonSR copies the file into its own folder. Nothing is run while adding it.";
        spec.bodyTone = Tone::Secondary;
    }
    spec.primary = risky ? L"Add anyway" : L"Add";
    spec.secondary = L"Cancel";
    spec.onPrimary = [this, path] { BeginImport(path); };
    spec.onCopy = [this](const std::wstring& t) { CopyToClipboard(hwnd_, t); };
    view_.ShowDialog(std::move(spec));
}

void MainWindow::BeginImport(const std::wstring& path) {
    ModelStore* store = &Models();
    SetNeuralMessage(L"Adding the file...", Severity::Info);
    RunModelTask([this, store, path]() -> std::function<void()> {
        struct R {
            ModelInfo info;
            std::wstring err;
            ImportResult res = ImportResult::StoreError;
        };
        auto r = std::make_shared<R>();
        r->res = store->Import(path, r->info, r->err);
        return [this, r] {
            switch (r->res) {
            case ImportResult::Ok:
            case ImportResult::AlreadyInstalled:
                selectedModelId_ = r->info.id;
                RefreshModels();
                OnNeuralChange();
                SetNeuralMessage(r->res == ImportResult::Ok ? L"Added " + r->info.displayName + L"."
                                                            : r->info.displayName + L" is already added.",
                                 r->res == ImportResult::Ok ? Severity::Success : Severity::Info);
                break;
            default:
                SetNeuralMessage(L"Could not add the file: " + r->err, Severity::Error);
                break;
            }
        };
    });
}

void MainWindow::BeginVerify(size_t index) {
    if (index >= models_.size()) return;
    ModelStore* store = &Models();
    const ModelInfo m = models_[index];
    SetNeuralMessage(L"Checking " + m.displayName + L"...", Severity::Info);
    RunModelTask([this, store, m]() -> std::function<void()> {
        std::wstring err;
        const bool ok = store->Verify(m.id, err);
        return [this, m, ok, err] {
            if (ok) SetNeuralMessage(m.displayName + L": the stored file matches the checksum recorded when it was added.", Severity::Success);
            else SetNeuralMessage(m.displayName + L": " + err, Severity::Error);
        };
    });
}

void MainWindow::ConfirmRemove(size_t index) {
    if (index >= models_.size() || busy_ || view_.DialogOpen()) return;
    const ModelInfo m = models_[index];
    ui::ModalDialog::Spec spec;
    spec.title = L"Remove this model?";
    spec.body = m.displayName + L"\nThe copy stored by MoonSR is deleted. Your original file is not touched.";
    spec.primary = L"Remove";
    spec.secondary = L"Cancel";
    spec.onPrimary = [this, m] {
        ModelStore* store = &Models();
        RunModelTask([this, store, m]() -> std::function<void()> {
            std::wstring err;
            const bool ok = store->Remove(m.id, err);
            return [this, m, ok, err] {
                if (ok && selectedModelId_ == m.id) selectedModelId_.clear();
                RefreshModels();
                if (ok) SetNeuralMessage(L"Removed " + m.displayName + L".", Severity::Success);
                else SetNeuralMessage(L"Could not remove the model: " + err, Severity::Error);
            };
        });
    };
    view_.ShowDialog(std::move(spec));
}

// ---------------------------------------------------------------------------------------------------------------------
// MoonSR temporal upscaler
// ---------------------------------------------------------------------------------------------------------------------

bool MainWindow::AlsrSelected() const {
    const int sc = ui_.scaler->Selected();
    if (sc < 0) return false;
    const long long idx = ui_.scaler->ItemData(sc);
    return idx >= 0 && static_cast<size_t>(idx) < scalers_.size() && scalers_[static_cast<size_t>(idx)].kind == ScalerKind::MoonALSR;
}

AlsrSettings MainWindow::CurrentAlsr() const {
    AlsrSettings a;
    a.stability = static_cast<float>(ui_.alsrHistory->value()) / 100.0f;
    return a;
}

// The MoonSR card is shown only while the scaler is MoonSR.
void MainWindow::UpdateAlsrUi() {
    // Short description of the selected scaler under the "Scaler" row.
    if (ui_.scalerRow) {
        const wchar_t* d = L"The algorithm that enlarges the image";
        const int sc = ui_.scaler->Selected();
        const long long idx = sc >= 0 ? ui_.scaler->ItemData(sc) : -1;
        if (idx >= 0 && static_cast<size_t>(idx) < scalers_.size()) {
            switch (scalers_[static_cast<size_t>(idx)].kind) {
            case ScalerKind::Nearest: d = L"Copies the nearest pixel. Crisp pixel art at whole multiples; blocky otherwise. Cheapest."; break;
            case ScalerKind::Bilinear: d = L"Blends the 4 nearest pixels. Smooth but soft. Very cheap."; break;
            case ScalerKind::Bicubic: d = L"Catmull-Rom over 16 pixels. Sharper than bilinear with slight halos. Cheap."; break;
            case ScalerKind::Lanczos: d = L"Lanczos-3 over 36 pixels. The sharpest classic filter; can ring on hard edges. Cheap."; break;
            case ScalerKind::MoonSRSpatial: d = L"MoonSR's edge-aware single-frame upscaler. Works on any game, no motion needed."; break;
            case ScalerKind::MoonALSR: d = L"Temporal: rebuilds detail from several frames using estimated motion. Best on steady "
                                           L"camera movement; heaviest (about 0.75 ms at 1080p on an RTX 5060)."; break;
            default: break;
            }
        }
        if (ui_.scalerRow->desc != d) {
            ui_.scalerRow->desc = d;
            view_.RequestLayout();
        }
    }
    const bool alsr = AlsrSelected();
    const bool wasShown = ui_.alsrInfo->visible;
    ui_.alsrInfo->SetVisible(alsr);
    ui_.alsrHistoryRow->visible = alsr;
    ui_.alsrHistory->SetVisible(alsr);
    ui_.alsrHistory->SetEnabled(ui_.upscaling->checked());
    if (!alsr) ui_.alsrNote->SetVisible(false);
    if (wasShown != alsr) view_.RequestLayout();
}

// ---------------------------------------------------------------------------------------------------------------------
// Load order
// ---------------------------------------------------------------------------------------------------------------------

PipelineOrder MainWindow::CurrentOrder() const {
    PipelineOrder o;
    o.stages.clear();
    for (const auto& e : ui_.orderList->entries) o.stages.push_back(static_cast<PipelineStage>(e.id));
    return Engine::NormalizeOrder(o);
}

void MainWindow::SetOrderUi(const PipelineOrder& order) {
    auto& entries = ui_.orderList->entries;
    entries.clear();
    for (PipelineStage st : Engine::NormalizeOrder(order).stages) {
        ui::OrderList::Entry e;
        e.id = static_cast<int>(st);
        e.icon = StageIcon(st);
        e.name = Engine::StageName(st);
        e.desc = Engine::StageDescription(st);
        e.combo = ui_.stageGpu[static_cast<int>(st)];  // the GPU choice belongs to the stage, not to the slot
        entries.push_back(std::move(e));
    }
    RefreshOrderUi();
    view_.RequestLayout();
}

// Badges (whether each stage is on in its own page) and the plain-words explanation of the current order.
void MainWindow::RefreshOrderUi() {
    auto badge = [&](PipelineStage st, bool on) {
        ui_.orderList->SetBadge(static_cast<int>(st), on ? L"On" : L"Off", on, L"");
    };
    badge(PipelineStage::Upscaling, ui_.upscaling->checked());
    badge(PipelineStage::FrameGeneration, ui_.frameGen->checked());
    badge(PipelineStage::NeuralRendering, ui_.neural->checked());

    const PipelineOrder o = CurrentOrder();
    std::wstring t = L"Frames move through the enabled stages from top to bottom.";
    bool generated = false, upscaled = false;
    int activeCount = 0;
    for (size_t i = 0; i < o.stages.size(); ++i) {
        const PipelineStage stage = o.stages[i];
        const bool active = stage == PipelineStage::Upscaling ? ui_.upscaling->checked() :
                            stage == PipelineStage::FrameGeneration ? ui_.frameGen->checked() : ui_.neural->checked();
        t += std::format(L"\n\n{}. {}: ", i + 1, Engine::StageName(stage));
        if (!active) {
            t += L"Off; this stage is skipped.";
            continue;
        }
        ++activeCount;
        switch (stage) {
        case PipelineStage::Upscaling:
            t += generated
                ? L"Enlarges both the game's frames and the generated frames to fit the output."
                : L"Enlarges each game frame to fit the output before it reaches the next stage.";
            upscaled = true;
            break;
        case PipelineStage::FrameGeneration:
            t += upscaled
                ? L"Creates intermediate frames from the upscaled game frames. Working at the larger resolution uses more GPU time."
                : L"Creates intermediate frames from game-resolution images. Later stages process both real and generated frames.";
            generated = true;
            break;
        case PipelineStage::NeuralRendering:
            t += generated
                ? L"Enhances both real and generated frames; processing more frames can use more GPU time."
                : L"Enhances each game frame before passing it to the next enabled stage.";
            t += upscaled ? L" It receives the upscaled image." : L" It receives the game-resolution image.";
            t += L" Motion analysis uses the original captured frames.";
            break;
        }
    }
    if (!activeCount) t += L"\n\nNo processing stages are enabled.";
    ui_.orderExplain->SetText(t);
    view_.Invalidate();
}

void MainWindow::OnOrderChanged() {
    const PipelineOrder order = CurrentOrder();
    SetOrderUi(order);
    if (engineActive_) engine_.SetPipelineOrder(order);
}

}  // namespace moonsr::app
