// Windows Graphics Capture source for one top-level window.
//
// Threading model:
//   - Create, AcquireFrame, ReleaseFrame and the destructor run on the engine pipeline thread (COM MTA).
//   - The frame pool is free-threaded. Its FrameArrived handler and the item's Closed handler run on OS worker
//     threads and only touch a shared, reference-counted Signal (auto-reset event + atomic flags). They never touch
//     D3D or the pool.
//   - All pool access (TryGetNextFrame, Recreate, Close) and every frame object live on the pipeline thread.
//
// Frame ownership: zero-copy. AcquireFrame keeps the newest Direct3D11CaptureFrame open and hands out its surface
// texture; ReleaseFrame closes it, returning the buffer to the pool. Older frames found while draining are closed
// immediately and counted in droppedBefore. The pool has kBufferCount buffers so the OS can keep writing while the
// caller holds one. (The research note suggests copying the client sub-rect out of the frame; we skip that copy
// because the contract already lets the source own the texture until ReleaseFrame and the consumer crops through
// contentRect.)
#include "capture/capture_source.h"

#include "core/log.h"
#include "gpu/dpi_scope.h"

#include <unknwn.h>
#include <inspectable.h>
#include <dwmapi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>

namespace moonsr {

namespace {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::Windows::Foundation::Metadata::ApiInformation;

constexpr int32_t kBufferCount = 3;
constexpr wgd::DirectXPixelFormat kPoolFormat = wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr int64_t kOneMs100ns = 10'000; // TimeSpan units are 100 ns
constexpr double kCropRefreshMs = 250.0;
constexpr wchar_t kSessionClass[] = L"Windows.Graphics.Capture.GraphicsCaptureSession";

// State shared with OS callback threads. Kept alive by the handlers' captures, so a late callback after the source
// is destroyed is harmless.
struct Signal {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
    std::atomic<bool> closed{false};
    std::atomic<uint64_t> arrived{0};
    std::atomic<int64_t> lastArrivalQpc{0};
    ~Signal() {
        if (event) CloseHandle(event);
    }
};

// SystemRelativeTime is a QPC-based value expressed in 100 ns units. Convert back to raw QPC ticks without overflow.
int64_t SystemRelativeToQpc(int64_t t100ns) {
    const int64_t f = QpcFrequency();
    return (t100ns / 10'000'000) * f + (t100ns % 10'000'000) * f / 10'000'000;
}

bool IsDeviceLostCode(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

class WgcCapture final : public ICaptureSource {
public:
    WgcCapture(GpuDevice& device, HWND target, const CaptureOptions& options)
        : device_(device), target_(target), options_(options), signal_(std::make_shared<Signal>()) {}

    ~WgcCapture() override {
        ReleaseFrame();
        try {
            frameArrived_.revoke();
            itemClosed_.revoke();
            if (session_) session_.Close();
            if (pool_) pool_.Close();
        } catch (const winrt::hresult_error& e) {
            MSR_LOG_WARN(L"WGC: shutdown error {}: {}", HrToString(e.code()), std::wstring_view(e.message()));
        } catch (...) {
            MSR_LOG_WARN(L"WGC: unknown exception during shutdown");
        }
        session_ = nullptr;
        pool_ = nullptr;
        item_ = nullptr;
        d3dDevice_ = nullptr;
    }

    bool Init(std::wstring& error) {
        if (!signal_->event) {
            error = L"WGC: CreateEvent failed: " + HrToString(HRESULT_FROM_WIN32(GetLastError()));
            return false;
        }
        try {
            if (!wgc::GraphicsCaptureSession::IsSupported()) {
                error = L"Windows Graphics Capture is not supported on this system (requires Windows 10 1803+).";
                return false;
            }
            if (!IsWindow(target_)) {
                error = L"The target window does not exist.";
                return false;
            }

            // WinRT wrapper around our D3D11 device.
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
            HRESULT hr = device_.Device()->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
            if (FAILED(hr)) {
                error = L"WGC: QueryInterface(IDXGIDevice) failed: " + HrToString(hr);
                return false;
            }
            winrt::com_ptr<::IInspectable> inspectable;
            hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
            if (FAILED(hr)) {
                error = L"WGC: CreateDirect3D11DeviceFromDXGIDevice failed: " + HrToString(hr);
                return false;
            }
            d3dDevice_ = inspectable.as<wgd3d::IDirect3DDevice>();

            // Capture item for the window.
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            hr = interop->CreateForWindow(target_, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item_));
            if (FAILED(hr) || !item_) {
                error = L"WGC: CreateForWindow failed (window cannot be captured): " + HrToString(hr);
                return false;
            }

            poolSize_ = item_.Size();
            if (poolSize_.Width <= 0 || poolSize_.Height <= 0) poolSize_ = {1, 1};
            pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(d3dDevice_, kPoolFormat, kBufferCount, poolSize_);
            session_ = pool_.CreateCaptureSession(item_);

            std::shared_ptr<Signal> sig = signal_;
            frameArrived_ = pool_.FrameArrived(winrt::auto_revoke, [sig](auto&&, auto&&) {
                sig->lastArrivalQpc.store(QpcNow(), std::memory_order_relaxed);
                sig->arrived.fetch_add(1, std::memory_order_relaxed);
                SetEvent(sig->event);
            });
            itemClosed_ = item_.Closed(winrt::auto_revoke, [sig](auto&&, auto&&) {
                sig->closed.store(true);
                SetEvent(sig->event);
            });

            ApplySessionOptions();
            session_.StartCapture();
        } catch (const winrt::hresult_error& e) {
            error = std::format(L"WGC initialisation failed: {} {}", HrToString(e.code()), std::wstring_view(e.message()));
            return false;
        } catch (const std::exception& e) {
            error = L"WGC initialisation failed: std::exception ";
            for (const char* p = e.what(); *p; ++p) error.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
            return false;
        }
        MSR_LOG_INFO(L"WGC: capturing hwnd {:p}, item {}x{}, {} buffers", static_cast<void*>(target_), poolSize_.Width,
                     poolSize_.Height, kBufferCount);
        return true;
    }

    CaptureStatus AcquireFrame(uint32_t timeoutMs, CapturedFrame& out) override {
        out = CapturedFrame{};
        if (heldFrame_) {
            MSR_LOG_WARN(L"WGC: AcquireFrame called without ReleaseFrame; releasing the previous frame");
            ReleaseFrame();
        }
        try {
            if (pendingRecreate_) RecreatePool();

            const int64_t deadline = QpcNow() + static_cast<int64_t>(timeoutMs) * QpcFrequency() / 1000;
            wgc::Direct3D11CaptureFrame newest{nullptr};
            uint32_t discarded = 0;
            int64_t arrival = 0; // FrameArrived time of the newest drained frame
            for (;;) {
                if (signal_->closed.load() || !IsWindow(target_)) {
                    if (newest) newest.Close();
                    MSR_LOG_INFO(L"WGC: target window closed");
                    return CaptureStatus::TargetLost;
                }
                // Drain: latest frame wins.
                while (wgc::Direct3D11CaptureFrame f = pool_.TryGetNextFrame()) {
                    ++sequence_;
                    if (newest) {
                        newest.Close();
                        ++discarded;
                    }
                    newest = std::move(f);
                }
                if (newest) {
                    // Content larger than the pool surface: the frame is clipped (incomplete). Drop it, recreate the
                    // pool now (nothing is held) and keep waiting for a full frame at the new size.
                    const winrt::Windows::Graphics::SizeInt32 cs = newest.ContentSize();
                    if (cs.Width > poolSize_.Width || cs.Height > poolSize_.Height) {
                        MSR_LOG_INFO(L"WGC: content grew {}x{} -> {}x{}, recreating frame pool", poolSize_.Width,
                                     poolSize_.Height, cs.Width, cs.Height);
                        newest.Close();
                        newest = nullptr;
                        ++discarded;
                        pendingSize_ = cs;
                        RecreatePool();
                        continue;
                    }
                    arrival = signal_->lastArrivalQpc.load(std::memory_order_relaxed);
                    break;
                }

                const int64_t remaining = deadline - QpcNow();
                if (remaining <= 0) {
                    const HRESULT ds = device_.DeviceStatus();
                    if (ds != S_OK) {
                        MSR_LOG_ERROR(L"WGC: device lost: {}", HrToString(ds));
                        return CaptureStatus::DeviceLost;
                    }
                    return CaptureStatus::Timeout;
                }
                // An event set for frames that were already drained causes one extra spin; harmless.
                const DWORD waitMs = static_cast<DWORD>((remaining * 1000 + QpcFrequency() - 1) / QpcFrequency());
                WaitForSingleObject(signal_->event, waitMs);
            }

            const HRESULT ds = device_.DeviceStatus();
            if (ds != S_OK) {
                newest.Close();
                MSR_LOG_ERROR(L"WGC: device lost: {}", HrToString(ds));
                return CaptureStatus::DeviceLost;
            }

            // Texture behind the frame surface.
            auto access = newest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            const HRESULT hr = access->GetInterface(IID_PPV_ARGS(heldTex_.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                newest.Close();
                MSR_LOG_ERROR(L"WGC: IDirect3DDxgiInterfaceAccess::GetInterface failed: {}", HrToString(hr));
                return IsDeviceLostCode(hr) ? CaptureStatus::DeviceLost : CaptureStatus::Error;
            }
            D3D11_TEXTURE2D_DESC desc;
            heldTex_->GetDesc(&desc);

            // Content shrank: the frame is complete inside the larger surface, so serve it cropped and recreate the
            // pool at the start of the next AcquireFrame. Recreate discards outstanding frames, so it must not run
            // while we hold one.
            const winrt::Windows::Graphics::SizeInt32 content = newest.ContentSize();
            if (content.Width != poolSize_.Width || content.Height != poolSize_.Height) {
                if (!pendingRecreate_ || pendingSize_.Width != content.Width || pendingSize_.Height != content.Height)
                    MSR_LOG_INFO(L"WGC: content shrank {}x{} -> {}x{}, recreating frame pool", poolSize_.Width,
                                 poolSize_.Height, content.Width, content.Height);
                pendingRecreate_ = content.Width > 0 && content.Height > 0;
                pendingSize_ = content;
            }
            const int32_t cw = std::min<int32_t>(content.Width, static_cast<int32_t>(desc.Width));
            const int32_t ch = std::min<int32_t>(content.Height, static_cast<int32_t>(desc.Height));
            if (static_cast<uint32_t>(std::max(cw, 0)) != lastContent_.width ||
                static_cast<uint32_t>(std::max(ch, 0)) != lastContent_.height ||
                QpcToMs(QpcNow() - cropEvaluatedQpc_) >= kCropRefreshMs)
                UpdateCrop(cw, ch);

            heldFrame_ = std::move(newest);
            out.texture = heldTex_.Get();
            out.format = desc.Format;
            out.textureSize = {desc.Width, desc.Height};
            out.contentRect = crop_;
            // Measured on Windows 11 26300 (RTX 5060, 144 Hz): SystemRelativeTime is ~one composition interval in
            // the FUTURE relative to FrameArrived (it behaves like the target vblank of the DWM frame), so it cannot be
            // used directly as "captured at". Use the FrameArrived time unless SystemRelativeTime is earlier. The
            // arrival time is read right after draining; it belongs to the newest frame except in the rare case that
            // another frame arrives within those few microseconds.
            const int64_t srt = SystemRelativeToQpc(heldFrame_.SystemRelativeTime().count());
            out.captureQpc = (arrival > 0 && arrival < srt) ? arrival : srt;
            out.osTargetQpc = srt;
            if (timingLogs_ < 3) {
                ++timingLogs_;
                MSR_LOG_DEBUG(L"WGC timing: SystemRelativeTime - arrival = {:.3f} ms, acquire - arrival = {:.3f} ms",
                              QpcToMs(srt - arrival), QpcToMs(QpcNow() - arrival));
            }
            out.sequence = sequence_;
            out.droppedBefore = discarded;
            return CaptureStatus::Ok;
        } catch (const winrt::hresult_error& e) {
            ReleaseFrame();
            const HRESULT code = e.code();
            if (IsDeviceLostCode(code) || device_.DeviceStatus() != S_OK) {
                MSR_LOG_ERROR(L"WGC: device lost during AcquireFrame: {}", HrToString(code));
                return CaptureStatus::DeviceLost;
            }
            if (signal_->closed.load() || !IsWindow(target_)) return CaptureStatus::TargetLost;
            MSR_LOG_ERROR(L"WGC: AcquireFrame failed: {} {}", HrToString(code), std::wstring_view(e.message()));
            return CaptureStatus::Error;
        } catch (...) {
            ReleaseFrame();
            MSR_LOG_ERROR(L"WGC: AcquireFrame failed with an unknown exception");
            return CaptureStatus::Error;
        }
    }

    void ReleaseFrame() override {
        heldTex_.Reset();
        if (!heldFrame_) return;
        try {
            heldFrame_.Close();
        } catch (const winrt::hresult_error& e) {
            MSR_LOG_WARN(L"WGC: frame Close failed: {}", HrToString(e.code()));
        }
        heldFrame_ = nullptr;
    }

    const wchar_t* MethodName() const override { return L"Windows Graphics Capture"; }

private:
    void ApplySessionOptions() {
        if (ApiInformation::IsPropertyPresent(kSessionClass, L"IsCursorCaptureEnabled"))
            session_.IsCursorCaptureEnabled(options_.captureCursor);
        else
            MSR_LOG_INFO(L"WGC: IsCursorCaptureEnabled not available on this OS build");

        if (options_.hideCaptureBorder) {
            if (ApiInformation::IsPropertyPresent(kSessionClass, L"IsBorderRequired")) {
                // Unpackaged apps may be denied; the setter then succeeds but is ignored, or throws on some builds.
                try {
                    session_.IsBorderRequired(false);
                } catch (const winrt::hresult_error& e) {
                    MSR_LOG_WARN(L"WGC: IsBorderRequired(false) rejected: {}", HrToString(e.code()));
                }
            } else {
                MSR_LOG_INFO(L"WGC: IsBorderRequired not available; the yellow capture border will show");
            }
        }

        // 24H2+: without MinUpdateInterval the compositor caps delivery near 60 fps. 1 ms is the smallest value that
        // behaves as "uncapped" in the reports collected in the research note; never below 1 ms.
        if (ApiInformation::IsPropertyPresent(kSessionClass, L"MinUpdateInterval")) {
            int64_t interval = kOneMs100ns;
            if (options_.maxFps > 0.0)
                interval = std::max<int64_t>(kOneMs100ns, static_cast<int64_t>(10'000'000.0 / options_.maxFps));
            session_.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{interval});
            MSR_LOG_INFO(L"WGC: MinUpdateInterval = {} us", interval / 10);
        } else {
            MSR_LOG_INFO(L"WGC: MinUpdateInterval not available (pre-24H2); delivery may be capped near 60 fps");
        }
    }

    void RecreatePool() {
        pendingRecreate_ = false;
        // Drain anything still queued at the old size; Recreate would discard it anyway.
        while (wgc::Direct3D11CaptureFrame f = pool_.TryGetNextFrame()) {
            ++sequence_;
            f.Close();
        }
        pool_.Recreate(d3dDevice_, kPoolFormat, kBufferCount, pendingSize_);
        poolSize_ = pendingSize_;
        lastContent_ = {};
    }

    // Client-area crop in texture coordinates. The WGC window content starts at the DWM extended frame bounds (the
    // visible window rect without the invisible resize borders); the client rect is located relative to it.
    void UpdateCrop(int32_t contentW, int32_t contentH) {
        cropEvaluatedQpc_ = QpcNow();
        lastContent_ = {static_cast<uint32_t>(std::max(contentW, 0)), static_cast<uint32_t>(std::max(contentH, 0))};
        RectI full{0, 0, std::max(contentW, 0), std::max(contentH, 0)};
        if (!options_.clientAreaOnly) {
            crop_ = full;
            return;
        }
        ScopedPerMonitorDpi dpi; // DWM frame bounds are always physical; make the client rect physical as well.
        RECT frame{};
        if (FAILED(DwmGetWindowAttribute(target_, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
            !GetWindowRect(target_, &frame)) {
            crop_ = full;
            return;
        }
        RECT client{};
        POINT origin{0, 0};
        if (!GetClientRect(target_, &client) || !ClientToScreen(target_, &origin)) {
            crop_ = full;
            return;
        }
        // Apply the non-client insets (title bar, borders) to the frame's own content size rather than using the
        // window's current client size: during a resize the window is already at its new size while the frame still
        // has the old content, and the insets stay valid for both.
        const int32_t insetL = std::max<int32_t>(0, origin.x - frame.left);
        const int32_t insetT = std::max<int32_t>(0, origin.y - frame.top);
        const int32_t insetR = std::max<int32_t>(0, frame.right - (origin.x + client.right));
        const int32_t insetB = std::max<int32_t>(0, frame.bottom - (origin.y + client.bottom));
        RectI r;
        r.left = std::min(insetL, full.right);
        r.top = std::min(insetT, full.bottom);
        r.right = std::max(r.left, full.right - insetR);
        r.bottom = std::max(r.top, full.bottom - insetB);
        if (r.Empty()) r = full; // e.g. minimised / degenerate client area
        if (r != crop_)
            MSR_LOG_DEBUG(L"WGC: crop [{},{})x[{},{}) of content {}x{} (frame bounds {}x{})", r.left, r.right, r.top,
                          r.bottom, contentW, contentH, frame.right - frame.left, frame.bottom - frame.top);
        crop_ = r;
    }

    GpuDevice& device_;
    HWND target_;
    CaptureOptions options_;
    std::shared_ptr<Signal> signal_;

    wgd3d::IDirect3DDevice d3dDevice_{nullptr};
    wgc::GraphicsCaptureItem item_{nullptr};
    wgc::Direct3D11CaptureFramePool pool_{nullptr};
    wgc::GraphicsCaptureSession session_{nullptr};
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived_;
    wgc::GraphicsCaptureItem::Closed_revoker itemClosed_;

    winrt::Windows::Graphics::SizeInt32 poolSize_{};
    winrt::Windows::Graphics::SizeInt32 pendingSize_{};
    bool pendingRecreate_ = false;

    wgc::Direct3D11CaptureFrame heldFrame_{nullptr};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> heldTex_;
    uint64_t sequence_ = 0;
    int timingLogs_ = 0;

    RectI crop_;
    Size lastContent_;
    int64_t cropEvaluatedQpc_ = 0;
};

} // namespace

std::unique_ptr<ICaptureSource> CreateWgcCapture(GpuDevice& device, HWND target, const CaptureOptions& options,
                                                 std::wstring& error) {
    error.clear();
    auto capture = std::make_unique<WgcCapture>(device, target, options);
    if (!capture->Init(error)) {
        MSR_LOG_ERROR(L"{}", error);
        return nullptr;
    }
    return capture;
}

} // namespace moonsr
