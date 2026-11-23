// Presenter: borderless, non-activating output window covering one monitor + flip-model swapchain.
//
// Threading model: created, used and destroyed on one thread (the engine pipeline thread). The window is owned by
// that thread, so its messages are dispatched by PumpMessages() on that thread. The window procedure only sets flags
// on the PresenterImpl; it never touches D3D. The window class is shared by all presenters in the process and is
// reference counted under a mutex (presenters may live on different threads).
//
// Z-order (keepAbove): the window is NOT topmost. It keeps itself directly above the game window by inserting itself
// after the window that is currently just above the game (GetWindow(game, GW_HWNDPREV)), or at the top of its band
// when the game is the top window. Re-checked when an out-of-context WinEvent hook reports a foreground change or a
// minimise/show/hide/cloak of the game, and polled every kRestackPollMs as a safety net (hooks can be missed, e.g.
// across integrity levels). WinEvent callbacks run on the thread that installed them, inside PeekMessage, i.e. in
// PumpMessages(); they only set a flag. Only our own window is ever moved; the game window is never touched.
// Rejected alternative: making the game window our owner (GWLP_HWNDPARENT). A cross-process owner relationship
// attaches the two threads' input queues, so a hung game could stall the pipeline thread, and the system would
// also destroy our window together with the game's and hide it with the game's owned popups.
//
// Back buffer: D3D11 flip-model swapchains always expose the current back buffer as buffer 0 (the runtime renames it
// on Present), so a single RTV created once on buffer 0 stays valid. Present() unbinds it from the pipeline; callers
// rebind it every frame (ScalePass does). After Present we DiscardView it, which is the correct hint for FLIP_DISCARD.
#include "present/presenter.h"

#include <algorithm>

#include "core/log.h"
#include "gpu/dpi_scope.h"

#include <dwmapi.h>

#include <mutex>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

using Microsoft::WRL::ComPtr;

namespace moonsr {

namespace {

constexpr wchar_t kClassName[] = L"MoonSR.OutputWindow";
// Three buffers: one on screen, one queued for the next refresh, one free to render into. With only two and V-Sync
// on, a frame cannot be drawn until the queued one has flipped, so the GPU sits idle until the vblank and any frame
// whose GPU work (e.g. neural rendering + optical flow, ~7 ms at 1080p on an RTX 5060) is longer than one refresh
// slips to every second refresh: 60 fps x2 on a 144 Hz display measured ~94 fps instead of 120. The waiting paths
// still use a maximum frame latency of 1 (see CreateSwapchain), so this adds a render target, not queued latency.
constexpr UINT kBufferCount = 3;

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr ULONGLONG kRestackPollMs = 100;
constexpr int kRestackWarnPerSecond = 20; // more restacks than this per second: something fights us for the slot

std::mutex g_classMutex;
int g_classRefs = 0;

LRESULT CALLBACK OutputWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void CALLBACK ZOrderWinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                                 DWORD eventThread, DWORD eventTime);

class PresenterImpl;
// Presenters with z-order hooks on this thread. WinEvent callbacks arrive on the installing thread, so no lock.
thread_local std::vector<PresenterImpl*> t_zorderPresenters;

bool IsTopmost(HWND hwnd) { return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; }

bool AcquireWindowClass(std::wstring& error) {
    std::lock_guard lock(g_classMutex);
    if (g_classRefs == 0) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OutputWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) {
            const DWORD e = GetLastError();
            if (e != ERROR_CLASS_ALREADY_EXISTS) {
                error = L"RegisterClassExW failed: " + HrToString(HRESULT_FROM_WIN32(e));
                return false;
            }
        }
    }
    ++g_classRefs;
    return true;
}

void ReleaseWindowClass() {
    std::lock_guard lock(g_classMutex);
    if (g_classRefs > 0 && --g_classRefs == 0) UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

class PresenterImpl final : public Presenter {
public:
    PresenterImpl(GpuDevice& device, const PresentOptions& options) : device_(device), options_(options) {}

    ~PresenterImpl() override {
        // Unbind and flush so no pending reference to the back buffer outlives the swapchain.
        if (auto* ctx = device_.Context()) {
            ID3D11RenderTargetView* nullRtv = nullptr;
            ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
            ctx->Flush();
        }
        for (HWINEVENTHOOK h : hooks_)
            if (h) UnhookWinEvent(h);
        std::erase(t_zorderPresenters, this);
        rtv_.Reset();
        swapchain_.Reset(); // swapchain must go before its window
        if (waitable_) CloseHandle(waitable_);
        if (hwnd_) {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (classAcquired_) ReleaseWindowClass();
    }

    bool Init(std::wstring& error) {
        if (!options_.monitor) {
            error = L"Presenter: no output monitor specified.";
            return false;
        }
        ScopedPerMonitorDpi dpi; // window created per-monitor-v2 aware; rect in physical pixels

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(options_.monitor, &mi)) {
            error = L"Presenter: GetMonitorInfo failed (monitor disconnected?)";
            return false;
        }
        monitorRect_ = mi.rcMonitor;
        size_ = {static_cast<uint32_t>(monitorRect_.right - monitorRect_.left),
                 static_cast<uint32_t>(monitorRect_.bottom - monitorRect_.top)};
        if (size_.Empty()) {
            error = L"Presenter: monitor has an empty rectangle.";
            return false;
        }

        if (!AcquireWindowClass(error)) return Fail(error);
        classAcquired_ = true;

        // Layered (kept fully opaque, which per the research note does not prevent DirectFlip) is required for
        // WS_EX_TRANSPARENT to make the window click-through for other processes' windows.
        DWORD ex = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP;
        if (options_.topmost) ex |= WS_EX_TOPMOST;
        if (options_.clickThrough) ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT;

        hwnd_ = CreateWindowExW(ex, kClassName, L"MoonSR Output", WS_POPUP, monitorRect_.left, monitorRect_.top,
                                static_cast<int>(size_.width), static_cast<int>(size_.height), nullptr, nullptr,
                                GetModuleHandleW(nullptr), this);
        if (!hwnd_) {
            error = L"Presenter: CreateWindowExW failed: " + HrToString(HRESULT_FROM_WIN32(GetLastError()));
            return Fail(error);
        }
        if (options_.clickThrough && !SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA))
            MSR_LOG_WARN(L"Presenter: SetLayeredWindowAttributes failed: {}",
                         HrToString(HRESULT_FROM_WIN32(GetLastError())));

        if (options_.excludeFromCapture && !SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE))
            MSR_LOG_WARN(L"Presenter: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed: {} - continuing; the "
                         L"output window may appear in screen captures",
                         HrToString(HRESULT_FROM_WIN32(GetLastError())));

        if (!CreateSwapchain(error)) return Fail(error);

        if (options_.keepAbove) keepAbove_ = GetAncestor(options_.keepAbove, GA_ROOT);
        if (options_.topmost || !keepAbove_) {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            if (!SetWindowPos(hwnd_, options_.topmost ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW))
                MSR_LOG_WARN(L"Presenter: SetWindowPos (initial z-order) failed: {}",
                             HrToString(HRESULT_FROM_WIN32(GetLastError())));
            shown_ = true;
        } else {
            InstallZOrderHooks();
            Restack(); // shows the window directly above the game (or keeps it hidden if the game is minimised)
        }

        MSR_LOG_INFO(L"Presenter: {}x{} at ({},{}), tearing {}, vsync {}, click-through {}, exclude-from-capture {}, "
                     L"z-order {}",
                     size_.width, size_.height, monitorRect_.left, monitorRect_.top, tearing_ ? L"yes" : L"no",
                     options_.vsync ? L"on" : L"off", options_.clickThrough ? L"on" : L"off",
                     options_.excludeFromCapture ? L"on" : L"off",
                     options_.topmost ? L"topmost" : (keepAbove_ ? L"above the captured window" : L"normal"));
        return true;
    }

    // Called from ZOrderWinEventProc (same thread, inside PeekMessage).
    void OnWinEvent(DWORD event, HWND hwnd, LONG idObject) {
        if (event == EVENT_SYSTEM_FOREGROUND) {
            restackDirty_ = true; // a newly activated window moves to the top; the game may now be above us
        } else if (idObject == OBJID_WINDOW && hwnd == keepAbove_) {
            restackDirty_ = true; // game minimised/restored, shown/hidden, cloaked/uncloaked
        }
    }

    Size BackBufferSize() const override { return size_; }
    DXGI_FORMAT BackBufferFormat() const override { return kFormat; }
    ID3D11RenderTargetView* BackBufferRtv() override { return rtv_.Get(); }
    HANDLE FrameLatencyWaitable() const override { return waitable_; }
    HWND Window() const override { return hwnd_; }
    void SetVsync(bool vsync) override { options_.vsync = vsync; }
    bool FrameStatistics(DXGI_FRAME_STATISTICS& statistics) const override {
        return swapchain_ && SUCCEEDED(swapchain_->GetFrameStatistics(&statistics));
    }

    PresentStatus Present() override {
        if (!swapchain_) return PresentStatus::Error;
        UINT interval = 1, flags = 0;
        if (!options_.vsync) {
            interval = 0;
            if (tearing_) flags = DXGI_PRESENT_ALLOW_TEARING;
        }
        const HRESULT hr = swapchain_->Present(interval, flags);
        device_.Context()->DiscardView(rtv_.Get());
        if (hr == S_OK) return PresentStatus::Ok;
        if (hr == DXGI_STATUS_OCCLUDED) return PresentStatus::Occluded;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
            hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
            MSR_LOG_ERROR(L"Presenter: Present: device lost {} (reason {})", HrToString(hr),
                          HrToString(device_.DeviceStatus()));
            return PresentStatus::DeviceLost;
        }
        if (SUCCEEDED(hr)) return PresentStatus::Ok; // other success codes (e.g. DXGI_STATUS_*) are not fatal
        MSR_LOG_ERROR(L"Presenter: Present failed: {}", HrToString(hr));
        return PresentStatus::Error;
    }

    bool PumpMessages() override {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                // Not re-posted: a re-posted WM_QUIT would be seen by (and invalidate) every later presenter created on
                // this thread. The engine thread has no other message loop that needs it; returning false is enough.
                invalid_ = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (displayChanged_) {
            displayChanged_ = false;
            if (MonitorChanged()) invalid_ = true;
        }
        if (keepAbove_ && !options_.topmost && !invalid_) {
            const ULONGLONG now = GetTickCount64();
            if (restackDirty_ || now - lastRestackCheck_ >= kRestackPollMs) {
                restackDirty_ = false;
                lastRestackCheck_ = now;
                Restack();
            }
        }
        return !invalid_ && hwnd_ != nullptr && IsWindow(hwnd_);
    }

    // Called from the window procedure (same thread).
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST:
            if (options_.clickThrough) return HTTRANSPARENT;
            break;
        case WM_DISPLAYCHANGE: displayChanged_ = true; break;
        case WM_CLOSE: return 0; // only we destroy this window
        case WM_DESTROY: invalid_ = true; break;
        case WM_ERASEBKGND: return 1;
        default: break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

private:
    bool Fail(const std::wstring& error) {
        MSR_LOG_ERROR(L"{}", error);
        return false;
    }

    void InstallZOrderHooks() {
        t_zorderPresenters.push_back(this);
        DWORD pid = 0;
        GetWindowThreadProcessId(keepAbove_, &pid);
        constexpr DWORD kFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        // Foreground changes anywhere (the activated window moves to the top of the z-order).
        hooks_[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ZOrderWinEventProc, 0, 0,
                                    kFlags);
        // Game-process only: minimise/restore, show/hide, cloak/uncloak.
        if (pid) {
            hooks_[1] = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, ZOrderWinEventProc,
                                        pid, 0, kFlags);
            hooks_[2] = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, ZOrderWinEventProc, pid, 0, kFlags);
            hooks_[3] = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, ZOrderWinEventProc, pid, 0,
                                        kFlags);
        }
        int installed = 0;
        for (HWINEVENTHOOK h : hooks_) installed += h ? 1 : 0;
        if (installed < (pid ? 4 : 1))
            MSR_LOG_WARN(L"Presenter: {} of {} z-order WinEvent hooks installed; relying on the {} ms poll", installed,
                         pid ? 4 : 1, kRestackPollMs);
    }

    // True when the game window is something the user can currently see on this desktop.
    bool GameShowing() const {
        if (!IsWindowVisible(keepAbove_) || IsIconic(keepAbove_)) return false;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(keepAbove_, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            return false;
        return true;
    }

    // Puts the output window directly above keepAbove_ (and in the same topmost band), or hides it while the game
    // is not showing. Cheap when nothing changed: a few window-manager reads, no SetWindowPos.
    void Restack() {
        if (!IsWindow(keepAbove_)) {
            // The game window is gone; the engine notices and stops. Leave the window where it is.
            if (!gameGoneLogged_) {
                gameGoneLogged_ = true;
                MSR_LOG_INFO(L"Presenter: the window to keep above was destroyed; z-order tracking stopped");
            }
            return;
        }
        if (!GameShowing()) {
            if (shown_) {
                ShowWindow(hwnd_, SW_HIDE);
                shown_ = false;
                MSR_LOG_INFO(L"Presenter: captured window minimised, hidden or cloaked; output window hidden");
            }
            return;
        }
        const bool gameTop = IsTopmost(keepAbove_);
        HWND prev = GetWindow(keepAbove_, GW_HWNDPREV);
        // Already directly above, in the game's band. (A game that drops its topmost style leaves us topmost and
        // still adjacent; the band check catches that.)
        if (shown_ && prev == hwnd_ && IsTopmost(hwnd_) == gameTop) return;

        if (!shown_) {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            shown_ = true;
            MSR_LOG_INFO(L"Presenter: captured window showing; output window shown");
            prev = GetWindow(keepAbove_, GW_HWNDPREV); // showing may have moved us
            if (prev == hwnd_ && IsTopmost(hwnd_) == gameTop) return;
        }

        const bool selfTop = IsTopmost(hwnd_);
        constexpr UINT kSwp = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        HWND after;
        if (gameTop) {
            // Rare: the game made itself topmost. We must be topmost too to stay above it.
            if (!selfTop) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, kSwp);
            after = prev ? prev : HWND_TOPMOST;
        } else if (!prev || IsTopmost(prev)) {
            // The game is the top non-topmost window: take the top of the non-topmost band (never insert after a
            // topmost window, which would make us topmost).
            after = selfTop ? HWND_NOTOPMOST : HWND_TOP;
        } else {
            after = prev; // inserting after a non-topmost window also clears our topmost style if we had it
        }
        if (!SetWindowPos(hwnd_, after, 0, 0, 0, 0, kSwp)) {
            if (!restackFailLogged_) {
                restackFailLogged_ = true;
                MSR_LOG_WARN(L"Presenter: SetWindowPos (restack above the captured window) failed: {}",
                             HrToString(HRESULT_FROM_WIN32(GetLastError())));
            }
            return;
        }

        const ULONGLONG now = GetTickCount64();
        if (now - restackWindowStart_ >= 1000) {
            restackWindowStart_ = now;
            restackCount_ = 0;
        }
        if (++restackCount_ > kRestackWarnPerSecond && !restackFightLogged_) {
            restackFightLogged_ = true;
            MSR_LOG_WARN(L"Presenter: restacked above the captured window more than {} times in one second; another "
                         L"window keeps taking the slot directly above it",
                         kRestackWarnPerSecond);
        }
    }

    bool CreateSwapchain(std::wstring& error) {
        IDXGIFactory2* factory = device_.Factory();
        BOOL allowTearing = FALSE;
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f5))) &&
            SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
            tearing_ = allowTearing != FALSE;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = size_.width;
        desc.Height = size_.height;
        desc.Format = kFormat;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBufferCount;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (tearing_) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        ComPtr<IDXGISwapChain1> sc1;
        HRESULT hr = factory->CreateSwapChainForHwnd(device_.Device(), hwnd_, &desc, nullptr, nullptr, &sc1);
        if (FAILED(hr)) {
            error = L"Presenter: CreateSwapChainForHwnd failed: " + HrToString(hr);
            return false;
        }
        hr = sc1.As(&swapchain_);
        if (FAILED(hr)) {
            error = L"Presenter: QueryInterface(IDXGISwapChain2) failed: " + HrToString(hr);
            return false;
        }

        ComPtr<IDXGIFactory2> parent;
        hr = swapchain_->GetParent(IID_PPV_ARGS(&parent));
        if (SUCCEEDED(hr)) hr = parent->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (FAILED(hr)) MSR_LOG_WARN(L"Presenter: MakeWindowAssociation failed: {}", HrToString(hr));

        hr = swapchain_->SetMaximumFrameLatency(1);
        if (FAILED(hr)) MSR_LOG_WARN(L"Presenter: SetMaximumFrameLatency(1) failed: {}", HrToString(hr));
        waitable_ = swapchain_->GetFrameLatencyWaitableObject();
        if (!waitable_) MSR_LOG_WARN(L"Presenter: no frame latency waitable object");

        ComPtr<ID3D11Texture2D> back;
        hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&back));
        if (FAILED(hr)) {
            error = L"Presenter: GetBuffer(0) failed: " + HrToString(hr);
            return false;
        }
        hr = device_.Device()->CreateRenderTargetView(back.Get(), nullptr, &rtv_);
        if (FAILED(hr)) {
            error = L"Presenter: CreateRenderTargetView(back buffer) failed: " + HrToString(hr);
            return false;
        }
        return true;
    }

    // True if our monitor disappeared or its rectangle changed (resolution, position, orientation).
    bool MonitorChanged() const {
        ScopedPerMonitorDpi dpi;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(options_.monitor, &mi)) {
            MSR_LOG_INFO(L"Presenter: output monitor removed");
            return true;
        }
        if (!EqualRect(&mi.rcMonitor, &monitorRect_)) {
            MSR_LOG_INFO(L"Presenter: output monitor changed to {}x{} at ({},{})", mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, mi.rcMonitor.left, mi.rcMonitor.top);
            return true;
        }
        return false;
    }

    GpuDevice& device_;
    PresentOptions options_;
    HWND hwnd_ = nullptr;
    bool classAcquired_ = false;
    RECT monitorRect_{};
    Size size_;
    bool tearing_ = false;
    ComPtr<IDXGISwapChain2> swapchain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    HANDLE waitable_ = nullptr;
    bool displayChanged_ = false;
    bool invalid_ = false;

    // Z-order tracking (keepAbove). Pipeline thread only.
    HWND keepAbove_ = nullptr;
    HWINEVENTHOOK hooks_[4]{};
    bool shown_ = false;
    bool restackDirty_ = false;
    ULONGLONG lastRestackCheck_ = 0;
    ULONGLONG restackWindowStart_ = 0;
    int restackCount_ = 0;
    bool restackFailLogged_ = false, restackFightLogged_ = false, gameGoneLogged_ = false;
};

void CALLBACK ZOrderWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    for (PresenterImpl* p : t_zorderPresenters) p->OnWinEvent(event, hwnd, idObject);
}

LRESULT CALLBACK OutputWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    if (auto* self = reinterpret_cast<PresenterImpl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
        return self->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

std::unique_ptr<Presenter> Presenter::Create(GpuDevice& device, const PresentOptions& options, std::wstring& error) {
    error.clear();
    auto p = std::make_unique<PresenterImpl>(device, options);
    if (!p->Init(error)) return nullptr;
    return p;
}

} // namespace moonsr
