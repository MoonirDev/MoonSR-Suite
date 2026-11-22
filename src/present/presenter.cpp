// Presenter: borderless, non-activating output window covering one monitor + flip-model swapchain.
//
// Threading model: created, used and destroyed on one thread (the engine pipeline thread). The window is owned by
// that thread, so its messages are dispatched by PumpMessages() on that thread. The window procedure only sets flags
// on the PresenterImpl; it never touches D3D. The window class is shared by all presenters in the process and is
// reference counted under a mutex (presenters may live on different threads).
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

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

using Microsoft::WRL::ComPtr;

namespace moonsr {

namespace {

constexpr wchar_t kClassName[] = L"MoonSR.OutputWindow";
constexpr UINT kBufferCount = 2;

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

std::mutex g_classMutex;
int g_classRefs = 0;

LRESULT CALLBACK OutputWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

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

        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, options_.topmost ? HWND_TOPMOST : HWND_TOP, monitorRect_.left, monitorRect_.top,
                     static_cast<int>(size_.width), static_cast<int>(size_.height), SWP_NOACTIVATE | SWP_SHOWWINDOW);

        MSR_LOG_INFO(L"Presenter: {}x{} at ({},{}), tearing {}, vsync {}, click-through {}, exclude-from-capture {}",
                     size_.width, size_.height, monitorRect_.left, monitorRect_.top, tearing_ ? L"yes" : L"no",
                     options_.vsync ? L"on" : L"off", options_.clickThrough ? L"on" : L"off",
                     options_.excludeFromCapture ? L"on" : L"off");
        return true;
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
};

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
