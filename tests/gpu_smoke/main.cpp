// moonsr_gpu_smoke: console smoke test for src/gpu, src/capture and src/present.
//
//   moonsr_gpu_smoke                                 enumerate adapters, monitors, windows; create a device per HW adapter
//   moonsr_gpu_smoke --capture "<title substring>" [seconds]   WGC capture of the first matching window (default 3 s)
//   moonsr_gpu_smoke --capture-self [seconds] [--uncapped]   WGC capture of a 960x540 animated window this test
//                                                    creates (Present interval 1, or 0 with --uncapped;
//                                                    --foreground tries to activate it, --resize / --shrink resize
//                                                    it to 1280x720 / 640x360 at 50 %, --close destroys it at 60 %, --hold <ms> holds each
//                                                    frame before ReleaseFrame to provoke drops)
//   moonsr_gpu_smoke --present [seconds] [--vsync] [--nowait] [--adapter <index>] [--monitor <index>]
//                                                    Presenter on the primary (or chosen) monitor (default 2 s); the
//                                                    device is on the monitor's adapter unless --adapter overrides it
//
// Logs go to gpu_smoke.log in the working directory.
#include "capture/capture_source.h"
#include "core/log.h"
#include "gpu/gpu_device.h"
#include "present/presenter.h"

#include <dwmapi.h>
#include <fcntl.h>
#include <io.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <functional>
#include <string>
#include <thread>

using namespace moonsr;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* FlName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
    case D3D_FEATURE_LEVEL_12_2: return L"12_2";
    case D3D_FEATURE_LEVEL_12_1: return L"12_1";
    case D3D_FEATURE_LEVEL_12_0: return L"12_0";
    case D3D_FEATURE_LEVEL_11_1: return L"11_1";
    case D3D_FEATURE_LEVEL_11_0: return L"11_0";
    case D3D_FEATURE_LEVEL_10_1: return L"10_1";
    case D3D_FEATURE_LEVEL_10_0: return L"10_0";
    default: return L"none";
    }
}

const wchar_t* VendorName(Vendor v) {
    switch (v) {
    case Vendor::Nvidia: return L"NVIDIA";
    case Vendor::Amd: return L"AMD";
    case Vendor::Intel: return L"Intel";
    case Vendor::Microsoft: return L"Microsoft";
    default: return L"Unknown";
    }
}

std::wstring LuidStr(const LUID& l) {
    return std::format(L"{:08X}:{:08X}", static_cast<uint32_t>(l.HighPart), l.LowPart);
}

const wchar_t* YesNo(bool b) { return b ? L"yes" : L"no"; }

void Print(const std::wstring& s) {
    fputws(s.c_str(), stdout);
    fflush(stdout);
}

void PrintAdapters() {
    const auto adapters = GpuDevice::EnumerateAdapters();
    Print(std::format(L"== Adapters ({}) ==\n", adapters.size()));
    for (size_t i = 0; i < adapters.size(); ++i) {
        const auto& a = adapters[i];
        const uint64_t v = a.driverVersion;
        Print(std::format(L"[{}] {}\n"
                          L"    luid={} vendor={} (0x{:04X}) device=0x{:04X}\n"
                          L"    dedicatedVRAM={} MiB sharedSys={} MiB driver={}.{}.{}.{}\n"
                          L"    software={} integrated={} outputs={}\n"
                          L"    maxFL={} compute={} tearing={} R16F_RT={} minPrecision16={}\n",
                          i, a.name, LuidStr(a.luid), VendorName(a.vendor), a.vendorId, a.deviceId,
                          a.dedicatedVideoMemory >> 20, a.sharedSystemMemory >> 20, (v >> 48) & 0xFFFF,
                          (v >> 32) & 0xFFFF, (v >> 16) & 0xFFFF, v & 0xFFFF, YesNo(a.isSoftware),
                          YesNo(a.isIntegrated), a.outputCount, FlName(a.maxFeatureLevel), YesNo(a.supportsCompute),
                          YesNo(a.supportsTearing), YesNo(a.supportsR16FRenderTarget), YesNo(a.supportsMinPrecision16)));
    }
}

void PrintMonitors() {
    const auto monitors = EnumerateMonitors();
    Print(std::format(L"== Monitors ({}) ==\n", monitors.size()));
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& m = monitors[i];
        Print(std::format(L"[{}] {} '{}' rect=({},{})-({},{}) {}x{} refresh={:.4f} Hz adapter={} primary={} hdr={}\n", i,
                          m.deviceName, m.friendlyName, m.desktopRect.left, m.desktopRect.top, m.desktopRect.right,
                          m.desktopRect.bottom, m.desktopRect.right - m.desktopRect.left,
                          m.desktopRect.bottom - m.desktopRect.top, m.refreshHz, LuidStr(m.adapterLuid),
                          YesNo(m.primary), YesNo(m.hdrEnabled)));
    }
}

void PrintDwmTiming() {
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof(ti);
    const HRESULT hr = DwmGetCompositionTimingInfo(nullptr, &ti);
    if (FAILED(hr)) {
        Print(L"== DWM timing: DwmGetCompositionTimingInfo failed " + HrToString(hr) + L"\n");
        return;
    }
    auto rate = [](const UNSIGNED_RATIO& r) {
        return r.uiDenominator ? static_cast<double>(r.uiNumerator) / r.uiDenominator : 0.0;
    };
    Print(std::format(L"== DWM timing: rateRefresh={:.4f} Hz rateCompose={:.4f} Hz qpcRefreshPeriod={:.4f} ms ==\n",
                      rate(ti.rateRefresh), rate(ti.rateCompose), QpcToMs(static_cast<int64_t>(ti.qpcRefreshPeriod))));
}

void PrintWindows() {
    const auto windows = EnumerateCaptureWindows();
    Print(std::format(L"== Capture windows ({}) ==\n", windows.size()));
    for (const auto& w : windows)
        Print(std::format(L"  hwnd={:p} pid={} exe={} client={}x{} '{}'\n", static_cast<void*>(w.hwnd), w.processId,
                          w.exeName.empty() ? L"?" : w.exeName, w.clientSize.width, w.clientSize.height, w.title));
}

void CreateDevicesOnAllHardware() {
    Print(L"== Device creation per hardware adapter ==\n");
    for (const auto& a : GpuDevice::EnumerateAdapters()) {
        if (a.isSoftware) continue;
        std::wstring error;
        const int64_t t0 = QpcNow();
        auto dev = GpuDevice::Create(&a.luid, error);
        const double ms = QpcToMs(QpcNow() - t0);
        if (dev)
            Print(std::format(L"  OK   {} FL={} status=0x{:08X} ({:.1f} ms)\n", dev->Info().name,
                              FlName(dev->Device()->GetFeatureLevel()), static_cast<uint32_t>(dev->DeviceStatus()), ms));
        else
            Print(std::format(L"  FAIL {}: {}\n", a.name, error));
    }
}

// ---- self-rendered capture target -------------------------------------------------------------------------------

struct SelfTarget {
    std::atomic<HWND> hwnd{nullptr};
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> presents{0};
    bool uncapped = false;
    bool foreground = false;
    std::atomic<bool> foregroundOk{false};
    std::thread thread;

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

    void Run() {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MoonSRSmokeTarget";
        RegisterClassExW(&wc);
        RECT r{0, 0, 960, 540};
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        HWND h = CreateWindowExW(0, wc.lpszClassName, L"MoonSR smoke capture target", WS_OVERLAPPEDWINDOW, 100, 100,
                                 r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        ComPtr<IDXGIFactory2> factory;
        ComPtr<IDXGISwapChain1> sc;
        ComPtr<ID3D11RenderTargetView> rtv;
        bool ok = h && SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx)) &&
                  SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        if (ok) {
            DXGI_SWAP_CHAIN_DESC1 d{};
            d.Width = 960;
            d.Height = 540;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            d.BufferCount = 2;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            ComPtr<ID3D11Texture2D> bb;
            ok = SUCCEEDED(factory->CreateSwapChainForHwnd(dev.Get(), h, &d, nullptr, nullptr, &sc)) &&
                 SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))) &&
                 SUCCEEDED(dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv));
        }
        if (!ok) {
            failed = true;
            if (h) DestroyWindow(h);
            hwnd = HWND(nullptr);
            return;
        }
        ShowWindow(h, foreground ? SW_SHOW : SW_SHOWNA);
        if (foreground) foregroundOk = SetForegroundWindow(h) != FALSE;
        hwnd = h;
        const int64_t t0 = QpcNow();
        while (!stop) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            const float t = static_cast<float>(QpcToMs(QpcNow() - t0) / 1000.0);
            const float c[4] = {0.5f + 0.5f * std::sin(t * 3.0f), 0.5f + 0.5f * std::sin(t * 2.1f + 1.0f),
                                0.5f + 0.5f * std::sin(t * 1.3f + 2.0f), 1.0f};
            ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            ctx->ClearRenderTargetView(rtv.Get(), c);
            sc->Present(uncapped ? 0 : 1, 0); // interval 1: paced by the refresh of the monitor the window is on
            presents++;
        }
        rtv.Reset();
        sc.Reset();
        DestroyWindow(h);
    }
};

// ---- capture mode -------------------------------------------------------------------------------------------------

struct CaptureTestOptions {
    DWORD holdMs = 0;                       // simulate a slow consumer (frames held before release)
    std::function<void(double)> onProgress; // called with the elapsed fraction once per loop iteration
};

int RunCapture(HWND target, double seconds, const std::wstring& label, const CaptureTestOptions& to = {}) {
    std::wstring error;
    auto dev = GpuDevice::Create(nullptr, error);
    if (!dev) {
        Print(L"GpuDevice::Create failed: " + error + L"\n");
        return 1;
    }
    Print(std::format(L"== Capture '{}' hwnd={:p} on '{}' for {:.1f} s ==\n", label, static_cast<void*>(target),
                      dev->Info().name, seconds));
    CaptureOptions co;
    auto cap = CreateWgcCapture(*dev, target, co, error);
    if (!cap) {
        Print(L"CreateWgcCapture failed: " + error + L"\n");
        return 1;
    }
    uint64_t frames = 0, dropped = 0, timeouts = 0;
    double latencySum = 0.0, latencyMin = 1e9, latencyMax = -1e9;
    Size lastTex;
    RectI lastRect;
    const int64_t start = QpcNow();
    int64_t firstFrame = 0, lastFrameQpc = 0;
    CaptureStatus last = CaptureStatus::Ok;
    while (QpcToMs(QpcNow() - start) < seconds * 1000.0) {
        if (to.onProgress) to.onProgress(QpcToMs(QpcNow() - start) / (seconds * 1000.0));
        CapturedFrame f;
        last = cap->AcquireFrame(100, f);
        if (last == CaptureStatus::Timeout) {
            timeouts++;
            continue;
        }
        if (last != CaptureStatus::Ok) break;
        const int64_t now = QpcNow();
        const double lat = QpcToMs(now - f.captureQpc);
        latencySum += lat;
        latencyMin = std::min(latencyMin, lat);
        latencyMax = std::max(latencyMax, lat);
        if (frames == 0) firstFrame = now;
        lastFrameQpc = now;
        frames++;
        dropped += f.droppedBefore;
        if (!(f.textureSize == lastTex) || !(f.contentRect == lastRect)) {
            D3D11_TEXTURE2D_DESC td{};
            f.texture->GetDesc(&td);
            Print(std::format(L"  frame seq={} texture={}x{} fmt={} bind=0x{:X} contentRect=[{},{})x[{},{}) ({}x{})\n",
                              f.sequence, f.textureSize.width, f.textureSize.height, static_cast<int>(f.format),
                              td.BindFlags, f.contentRect.left, f.contentRect.right, f.contentRect.top,
                              f.contentRect.bottom, f.contentRect.Width(), f.contentRect.Height()));
            lastTex = f.textureSize;
            lastRect = f.contentRect;
        }
        if (to.holdMs) Sleep(to.holdMs);
        cap->ReleaseFrame();
    }
    const double elapsed = QpcToMs(QpcNow() - start) / 1000.0;
    const wchar_t* statusName[] = {L"Ok", L"Timeout", L"TargetLost", L"DeviceLost", L"Error"};
    const double steadyFps = frames > 1 ? (frames - 1) / (QpcToMs(lastFrameQpc - firstFrame) / 1000.0) : 0.0;
    Print(std::format(L"  elapsed={:.3f} s frames={} fps(overall)={:.2f} fps(first..last)={:.2f} dropped={} "
                      L"timeouts(100ms)={} lastStatus={}\n",
                      elapsed, frames, frames / elapsed, steadyFps, dropped, timeouts,
                      statusName[static_cast<int>(last)]));
    if (frames)
        Print(std::format(L"  latency (acquire time - captureQpc): mean={:.3f} ms min={:.3f} ms max={:.3f} ms\n",
                          latencySum / frames, latencyMin, latencyMax));
    cap.reset();
    return 0;
}

// ---- present mode -------------------------------------------------------------------------------------------------

int RunPresent(double seconds, bool vsync, bool wait, int adapterIndex, int monitorIndex) {
    MonitorInfo primary;
    const auto monitors = EnumerateMonitors();
    for (size_t i = 0; i < monitors.size(); ++i)
        if (monitorIndex >= 0 ? static_cast<int>(i) == monitorIndex : monitors[i].primary) primary = monitors[i];
    if (!primary.handle) {
        Print(L"No primary monitor found\n");
        return 1;
    }
    LUID luid = primary.adapterLuid; // default: the adapter driving the monitor (as the engine does)
    if (adapterIndex >= 0) {
        const auto adapters = GpuDevice::EnumerateAdapters();
        if (adapterIndex >= static_cast<int>(adapters.size())) {
            Print(L"Adapter index out of range\n");
            return 1;
        }
        luid = adapters[static_cast<size_t>(adapterIndex)].luid;
    }
    std::wstring error;
    auto dev = GpuDevice::Create(&luid, error);
    if (!dev) {
        Print(L"GpuDevice::Create failed: " + error + L"\n");
        return 1;
    }
    PresentOptions po;
    po.monitor = primary.handle;
    po.vsync = vsync;
    po.clickThrough = true;
    auto pres = Presenter::Create(*dev, po, error);
    if (!pres) {
        Print(L"Presenter::Create failed: " + error + L"\n");
        return 1;
    }
    const Size bb = pres->BackBufferSize();
    Print(std::format(L"== Present on {} ({}x{}, {:.3f} Hz) via '{}' for {:.1f} s, vsync={}, wait-on-waitable={}, "
                      L"tearing supported={} ==\n",
                      primary.deviceName, bb.width, bb.height, primary.refreshHz, dev->Info().name, seconds,
                      YesNo(vsync), YesNo(wait), YesNo(dev->Info().supportsTearing)));
    HANDLE waitable = pres->FrameLatencyWaitable();
    uint64_t ok = 0, occluded = 0, waitTimeouts = 0;
    PresentStatus bad = PresentStatus::Ok;
    bool pumpFailed = false;
    const int64_t start = QpcNow();
    while (QpcToMs(QpcNow() - start) < seconds * 1000.0) {
        if (wait && waitable && WaitForSingleObjectEx(waitable, 1000, TRUE) != WAIT_OBJECT_0) waitTimeouts++;
        if (!pres->PumpMessages()) {
            pumpFailed = true;
            break;
        }
        const float t = static_cast<float>(QpcToMs(QpcNow() - start) / 1000.0);
        const float c[4] = {0.5f + 0.5f * std::sin(t * 6.0f), 0.2f, 0.5f + 0.5f * std::cos(t * 4.0f), 1.0f};
        ID3D11RenderTargetView* rtv = pres->BackBufferRtv();
        dev->Context()->OMSetRenderTargets(1, &rtv, nullptr);
        dev->Context()->ClearRenderTargetView(rtv, c);
        const PresentStatus ps = pres->Present();
        if (ps == PresentStatus::Ok) ok++;
        else if (ps == PresentStatus::Occluded) occluded++;
        else {
            bad = ps;
            break;
        }
    }
    const double elapsed = QpcToMs(QpcNow() - start) / 1000.0;
    Print(std::format(L"  elapsed={:.3f} s presents ok={} ({:.1f}/s) occluded={} waitTimeouts={} fatal={} "
                      L"pumpFalse={}\n",
                      elapsed, ok, ok / elapsed, occluded, waitTimeouts,
                      bad == PresentStatus::Ok ? L"none" : (bad == PresentStatus::DeviceLost ? L"DeviceLost" : L"Error"),
                      YesNo(pumpFailed)));
    pres.reset();
    return 0;
}

bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    auto lower = [](std::wstring s) {
        for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
        return s;
    };
    return lower(hay).find(lower(needle)) != std::wstring::npos;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    InitLog(L"gpu_smoke.log");

    int rc = 0;
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    if (mode == L"--capture" && argc > 2) {
        const double secs = argc > 3 ? _wtof(argv[3]) : 3.0;
        HWND target = nullptr;
        std::wstring title;
        for (const auto& w : EnumerateCaptureWindows())
            if (ContainsNoCase(w.title, argv[2])) {
                target = w.hwnd;
                title = w.title;
                break;
            }
        if (!target) {
            Print(std::format(L"No capture window matches '{}'\n", argv[2]));
            rc = 1;
        } else {
            rc = RunCapture(target, secs > 0 ? secs : 3.0, title);
        }
    } else if (mode == L"--capture-self") {
        double secs = 3.0;
        SelfTarget st;
        CaptureTestOptions to;
        bool resize = false, shrink = false, close = false;
        for (int i = 2; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--uncapped") st.uncapped = true;
            else if (a == L"--foreground") st.foreground = true;
            else if (a == L"--resize") resize = true; // resize the target to 1280x720 client at 50 %
            else if (a == L"--shrink") shrink = true; // resize the target to 640x360 client at 50 %
            else if (a == L"--close") close = true;   // destroy the target at 60 % (expect TargetLost)
            else if (a == L"--hold" && i + 1 < argc) to.holdMs = static_cast<DWORD>(_wtoi(argv[++i]));
            else if (_wtof(a.c_str()) > 0) secs = _wtof(a.c_str());
        }
        bool resized = false;
        to.onProgress = [&](double frac) {
            if ((resize || shrink) && !resized && frac >= 0.5) {
                resized = true;
                RECT r{0, 0, resize ? 1280 : 640, resize ? 720 : 360};
                AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
                SetWindowPos(st.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                Print(resize ? L"  [resized target window to 1280x720 client]\n"
                             : L"  [resized target window to 640x360 client]\n");
            }
            if (close && !st.stop && frac >= 0.6) {
                st.stop = true;
                Print(L"  [closing target window]\n");
            }
        };
        st.thread = std::thread([&] { st.Run(); });
        while (!st.hwnd && !st.failed) Sleep(10);
        if (st.failed) {
            Print(L"Could not create the self-rendered target window\n");
            rc = 1;
        } else {
            Sleep(300); // let it present a few frames
            const uint64_t b0 = st.presents;
            const int64_t bt0 = QpcNow();
            Sleep(1000);
            Print(std::format(L"  target window presented {:.1f} frames/s before capture started (foreground "
                              L"requested={} granted={} isForeground={})\n",
                              (st.presents - b0) / (QpcToMs(QpcNow() - bt0) / 1000.0), YesNo(st.foreground),
                              YesNo(st.foregroundOk), YesNo(GetForegroundWindow() == st.hwnd)));
            const uint64_t p0 = st.presents;
            const int64_t t0 = QpcNow();
            rc = RunCapture(st.hwnd, secs > 0 ? secs : 3.0, L"self-rendered target", to);
            const double el = QpcToMs(QpcNow() - t0) / 1000.0;
            Print(std::format(L"  target window presented {:.1f} frames/s during the capture\n", (st.presents - p0) / el));
        }
        st.stop = true;
        st.thread.join();
    } else if (mode == L"--present") {
        double secs = 2.0;
        bool vsync = false, wait = true;
        int adapter = -1, monitor = -1;
        for (int i = 2; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--vsync") vsync = true;
            else if (a == L"--nowait") wait = false;
            else if (a == L"--adapter" && i + 1 < argc) adapter = _wtoi(argv[++i]);
            else if (a == L"--monitor" && i + 1 < argc) monitor = _wtoi(argv[++i]);
            else if (_wtof(a.c_str()) > 0) secs = _wtof(a.c_str());
        }
        rc = RunPresent(secs, vsync, wait, adapter, monitor);
    } else {
        PrintAdapters();
        PrintMonitors();
        PrintDwmTiming();
        PrintWindows();
        CreateDevicesOnAllHardware();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return rc;
}
