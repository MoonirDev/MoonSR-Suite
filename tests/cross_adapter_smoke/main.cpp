// moonsr_cross_adapter_smoke: correctness test and timing for CrossAdapterLink (src/gpu/cross_adapter.h).
//
//   moonsr_cross_adapter_smoke                       for every ordered pair of hardware adapters, with the automatic
//                                                    method and with MOONSR_XADAPTER_FORCE_CPU=1: transfer sub-rects of
//                                                    a pattern texture (BGRA8 then RGBA16F on the same link, with size
//                                                    changes mid-run), read back on the destination, compare bit-exact
//   moonsr_cross_adapter_smoke --bench [iterations]  also measure 1920x1080 transfers (default 300 iterations)
//   moonsr_cross_adapter_smoke --pair <src> <dst>    restrict to one pair (indices from the adapter list)
//   moonsr_cross_adapter_smoke --size <w> <h>        benchmark size (default 1920 1080)
//   moonsr_cross_adapter_smoke --debug-layers        enable the D3D12 debug layer and print D3D11 (Debug builds create
//                                                    GpuDevices with the D3D11 debug layer) and D3D12 info-queue
//                                                    warnings/errors after each pair; any message counts as a failure
//
// Timing definitions (QueryPerformanceCounter, CPU wall time):
//   serial call  : duration of Transfer() when the previous transfer has fully completed on the destination.
//   serial ready : from the start of Transfer() until a D3D11 event query issued on the destination right after it
//                  has completed (destination-ready latency, polled without flushing in a spin loop).
//   pipelined    : Transfer() + a 1x1 consumer copy + Flush on the destination, back to back, no waiting; "call" is
//                  the duration of Transfer(), "per frame" is total wall time / iterations including the final drain.
// Logs go to cross_adapter_smoke.log in the working directory. Exit code 0 = all correctness checks passed.
#include "core/log.h"
#include "gpu/cross_adapter.h"
#include "gpu/gpu_device.h"

#include <fcntl.h>
#include <io.h>

#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace moonsr;
using Microsoft::WRL::ComPtr;

namespace {

void Print(const std::wstring& s) {
    fputws(s.c_str(), stdout);
    fflush(stdout);
}

uint32_t Bpp(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4; }
const wchar_t* FmtName(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? L"RGBA16F" : L"BGRA8"; }

uint32_t Hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// Deterministic content for (x, y, seed). For RGBA16F every half avoids exponent 31 (no Inf/NaN), so a copy that
// canonicalises NaNs cannot cause a false failure; all other bit patterns, denormals included, are used.
void FillTexel(uint8_t* p, DXGI_FORMAT f, uint32_t x, uint32_t y, uint32_t seed) {
    const uint32_t a = Hash(x * 0x9E3779B1U ^ y * 0x85EBCA77U ^ seed * 0xC2B2AE3DU);
    if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const uint32_t b = Hash(a ^ 0x27D4EB2FU);
        uint16_t h[4] = {uint16_t(a), uint16_t(a >> 16), uint16_t(b), uint16_t(b >> 16)};
        for (auto& v : h)
            if (((v >> 10) & 0x1F) == 0x1F) v = static_cast<uint16_t>(v & ~0x4000);
        std::memcpy(p, h, 8);
    } else {
        std::memcpy(p, &a, 4);
    }
}

std::vector<uint8_t> MakePattern(DXGI_FORMAT f, uint32_t w, uint32_t h, uint32_t seed) {
    const uint32_t bpp = Bpp(f);
    std::vector<uint8_t> data(size_t(w) * h * bpp);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) FillTexel(&data[(size_t(y) * w + x) * bpp], f, x, y, seed);
    return data;
}

ComPtr<ID3D11Texture2D> CreateTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT f, D3D11_USAGE usage,
                                  UINT bind, UINT cpu) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = cpu;
    ComPtr<ID3D11Texture2D> t;
    const HRESULT hr = dev->CreateTexture2D(&d, nullptr, &t);
    if (FAILED(hr)) Print(std::format(L"  CreateTexture2D failed: {}\n", HrToString(hr)));
    return t;
}

struct Pending {
    ComPtr<ID3D11Texture2D> staging;
    uint32_t seed;
    RectI rect;
};

// Compares the readback of `p` with the pattern sub-rect. Returns true when bit-exact.
bool Verify(GpuDevice& dst, const Pending& p, DXGI_FORMAT f, uint32_t srcW, uint32_t srcH, std::wstring& why) {
    const auto pattern = MakePattern(f, srcW, srcH, p.seed);
    const uint32_t bpp = Bpp(f), w = uint32_t(p.rect.Width()), h = uint32_t(p.rect.Height());
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = dst.Context()->Map(p.staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        why = L"Map failed: " + HrToString(hr);
        return false;
    }
    bool ok = true;
    for (uint32_t y = 0; y < h && ok; ++y) {
        const uint8_t* got = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        const uint8_t* want = &pattern[((size_t(p.rect.top) + y) * srcW + uint32_t(p.rect.left)) * bpp];
        if (std::memcmp(got, want, size_t(w) * bpp) != 0) {
            for (uint32_t x = 0; x < w; ++x)
                if (std::memcmp(got + size_t(x) * bpp, want + size_t(x) * bpp, bpp) != 0) {
                    why = std::format(L"first mismatch at ({}, {}) of the {}x{} result (seed {})", x, y, w, h, p.seed);
                    break;
                }
            ok = false;
        }
    }
    dst.Context()->Unmap(p.staging.Get(), 0);
    return ok;
}

void SetForceCpu(bool on) { SetEnvironmentVariableW(L"MOONSR_XADAPTER_FORCE_CPU", on ? L"1" : nullptr); }

// Correctness for one link on one ordered pair. The same link is used for both formats (a format change mid-run).
bool RunCorrectness(GpuDevice& src, GpuDevice& dst, CrossAdapterLink& link) {
    constexpr uint32_t kW = 1920, kH = 1080;
    const RectI phases[] = {{37, 21, 37 + 1501, 21 + 903}, {0, 0, 1920, 1080}, {100, 50, 100 + 640, 50 + 360}};
    constexpr int kPerPhase = 4;
    bool allOk = true;
    uint32_t seed = 1;
    for (DXGI_FORMAT f : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        auto source = CreateTex(src.Device(), kW, kH, f, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0);
        if (!source) return false;
        std::vector<Pending> pending;
        bool ok = true;
        std::wstring why;
        for (const RectI& rect : phases) {
            for (int k = 0; k < kPerPhase && ok; ++k, ++seed) {
                // New content every iteration: a stale frame would fail the comparison.
                const auto pattern = MakePattern(f, kW, kH, seed);
                src.Context()->UpdateSubresource(source.Get(), 0, nullptr, pattern.data(), kW * Bpp(f), 0);
                std::wstring error;
                ID3D11Texture2D* out = link.Transfer(source.Get(), rect, error);
                if (!out) {
                    why = L"Transfer failed: " + error;
                    ok = false;
                    break;
                }
                D3D11_TEXTURE2D_DESC od{};
                out->GetDesc(&od);
                if (od.Width != uint32_t(rect.Width()) || od.Height != uint32_t(rect.Height()) || od.Format != f ||
                    !(od.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
                    why = std::format(L"returned texture {}x{} fmt {} bind 0x{:X} does not match", od.Width, od.Height,
                                      int(od.Format), od.BindFlags);
                    ok = false;
                    break;
                }
                // Read it back immediately (valid until the next Transfer), verify after the whole run so the
                // transfers are pipelined like in the engine.
                Pending p{CreateTex(dst.Device(), od.Width, od.Height, f, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ),
                          seed, rect};
                if (!p.staging) {
                    ok = false;
                    break;
                }
                dst.Context()->CopyResource(p.staging.Get(), out);
                pending.push_back(std::move(p));
            }
        }
        size_t verified = 0;
        for (const auto& p : pending) {
            if (!ok) break;
            if (!Verify(dst, p, f, kW, kH, why)) {
                ok = false;
                break;
            }
            ++verified;
        }
        Print(std::format(L"    {:<8} {}  ({} transfers verified bit-exact; rects 1501x903 @(37,21), 1920x1080, "
                          L"640x360 @(100,50)){}\n",
                          FmtName(f), ok ? L"PASS" : L"FAIL", verified, ok ? L"" : L"  -- " + why));
        allOk = allOk && ok;
    }
    return allOk;
}

struct Stats {
    double median = 0, p95 = 0, mean = 0;
};

Stats Summarize(std::vector<double>& v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.median = v[v.size() / 2];
    s.p95 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.95))];
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / double(v.size());
    return s;
}

bool WaitEvent(GpuDevice& dst, ID3D11Query* q) {
    dst.Context()->End(q);
    dst.Context()->Flush();
    const int64_t start = QpcNow();
    for (;;) {
        BOOL done = FALSE;
        const HRESULT hr = dst.Context()->GetData(q, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK && done) return true;
        if (FAILED(hr) || QpcToMs(QpcNow() - start) > 5000) return false;
        YieldProcessor();
    }
}

uint32_t g_benchW = 1920, g_benchH = 1080;

void RunBench(GpuDevice& src, GpuDevice& dst, bool forceCpu, int iterations, const std::wstring& pairName) {
    SetForceCpu(forceCpu);
    std::wstring error;
    auto link = CrossAdapterLink::Create(src, dst, error);
    SetForceCpu(false);
    if (!link) {
        Print(std::format(L"  bench: Create failed: {}\n", error));
        return;
    }
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    if (FAILED(dst.Device()->CreateQuery(&qd, &query))) return;
    const uint32_t kW = g_benchW, kH = g_benchH;
    const RectI rect{0, 0, int32_t(kW), int32_t(kH)};
    for (DXGI_FORMAT f : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        auto source = CreateTex(src.Device(), kW, kH, f, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0);
        auto sink = CreateTex(dst.Device(), 1, 1, f, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0);
        if (!source || !sink) return;
        const auto pattern = MakePattern(f, kW, kH, 7);
        src.Context()->UpdateSubresource(source.Get(), 0, nullptr, pattern.data(), kW * Bpp(f), 0);
        const D3D11_BOX one{0, 0, 0, 1, 1, 1};
        auto consume = [&](ID3D11Texture2D* out) {
            dst.Context()->CopySubresourceRegion(sink.Get(), 0, 0, 0, 0, out, 0, &one);
        };
        bool failed = false;
        // Warm-up (resource creation, driver paging).
        for (int i = 0; i < 20 && !failed; ++i) {
            ID3D11Texture2D* out = link->Transfer(source.Get(), rect, error);
            if (!out) failed = true;
            else consume(out);
        }
        if (failed || !WaitEvent(dst, query.Get())) {
            Print(std::format(L"  bench {} {}: failed ({})\n", FmtName(f), link->Method(), error));
            continue;
        }

        std::vector<double> sCall, sReady, pCall;
        sCall.reserve(iterations);
        sReady.reserve(iterations);
        pCall.reserve(iterations);
        for (int i = 0; i < iterations && !failed; ++i) {
            const int64_t t0 = QpcNow();
            ID3D11Texture2D* out = link->Transfer(source.Get(), rect, error);
            const int64_t t1 = QpcNow();
            if (!out) {
                failed = true;
                break;
            }
            consume(out);
            if (!WaitEvent(dst, query.Get())) failed = true;
            const int64_t t2 = QpcNow();
            sCall.push_back(QpcToMs(t1 - t0));
            sReady.push_back(QpcToMs(t2 - t0));
        }
        const int64_t pStart = QpcNow();
        for (int i = 0; i < iterations && !failed; ++i) {
            const int64_t t0 = QpcNow();
            ID3D11Texture2D* out = link->Transfer(source.Get(), rect, error);
            const int64_t t1 = QpcNow();
            if (!out) {
                failed = true;
                break;
            }
            consume(out);
            dst.Context()->Flush();
            pCall.push_back(QpcToMs(t1 - t0));
        }
        if (!failed && !WaitEvent(dst, query.Get())) failed = true;
        const double pTotal = QpcToMs(QpcNow() - pStart);
        if (failed) {
            Print(std::format(L"  bench {} {}: failed ({})\n", FmtName(f), link->Method(), error));
            continue;
        }
        const Stats c = Summarize(sCall), r = Summarize(sReady), pc = Summarize(pCall);
        Print(std::format(L"  BENCH | {} | {}x{} {} | {} | serial call med {:.3f} p95 {:.3f} | serial ready med {:.3f} p95 "
                          L"{:.3f} | pipelined call med {:.3f} p95 {:.3f} | pipelined per frame {:.3f} ms\n",
                          pairName, kW, kH, FmtName(f), link->Method(), c.median, c.p95, r.median, r.p95, pc.median, pc.p95,
                          pTotal / iterations));
    }
}

// Prints and clears stored debug-layer messages of severity warning or worse. Returns their number.
int DumpD3D11Messages(ID3D11Device* device, const wchar_t* who) {
    ComPtr<ID3D11InfoQueue> q;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&q)))) return 0;
    int count = 0;
    for (UINT64 i = 0, n = q->GetNumStoredMessages(); i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(q->GetMessage(i, nullptr, &len)) || len == 0) continue;
        std::vector<uint8_t> buf(len);
        auto* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (FAILED(q->GetMessage(i, m, &len))) continue;
        if (m->Severity > D3D11_MESSAGE_SEVERITY_WARNING) continue;
        ++count;
        Print(std::format(L"    D3D11 {} [{}]: {}\n", who, int(m->Severity),
                          std::wstring(m->pDescription, m->pDescription + strlen(m->pDescription))));
    }
    q->ClearStoredMessages();
    return count;
}

int DumpD3D12Messages(ID3D12Device* device, const wchar_t* who) {
    ComPtr<ID3D12InfoQueue> q;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&q)))) return 0;
    int count = 0;
    for (UINT64 i = 0, n = q->GetNumStoredMessages(); i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(q->GetMessage(i, nullptr, &len)) || len == 0) continue;
        std::vector<uint8_t> buf(len);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(q->GetMessage(i, m, &len))) continue;
        if (m->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
        ++count;
        Print(std::format(L"    D3D12 {} [{}]: {}\n", who, int(m->Severity),
                          std::wstring(m->pDescription, m->pDescription + strlen(m->pDescription))));
    }
    q->ClearStoredMessages();
    return count;
}

std::wstring DriverStr(uint64_t v) {
    return std::format(L"{}.{}.{}.{}", (v >> 48) & 0xFFFF, (v >> 32) & 0xFFFF, (v >> 16) & 0xFFFF, v & 0xFFFF);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    InitLog(L"cross_adapter_smoke.log");

    bool bench = false, debugLayers = false;
    int iterations = 300;
    int onlySrc = -1, onlyDst = -1;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--debug-layers") {
            debugLayers = true;
        } else if (a == L"--bench") {
            bench = true;
            if (i + 1 < argc && iswdigit(argv[i + 1][0])) iterations = std::max(10, _wtoi(argv[++i]));
        } else if (a == L"--size" && i + 2 < argc) {
            g_benchW = std::max(1, _wtoi(argv[++i]));
            g_benchH = std::max(1, _wtoi(argv[++i]));
        } else if (a == L"--pair" && i + 2 < argc) {
            onlySrc = _wtoi(argv[++i]);
            onlyDst = _wtoi(argv[++i]);
        }
    }

    if (debugLayers) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            Print(L"D3D12 debug layer enabled\n");
        } else {
            Print(L"D3D12 debug layer unavailable (Graphics Tools not installed?)\n");
        }
    }

    std::vector<AdapterInfo> adapters;
    for (auto& a : GpuDevice::EnumerateAdapters())
        if (!a.isSoftware) adapters.push_back(a);
    Print(std::format(L"== Hardware adapters ({}) ==\n", adapters.size()));
    for (size_t i = 0; i < adapters.size(); ++i)
        Print(std::format(L"[{}] {}  driver {}  {}\n", i, adapters[i].name, DriverStr(adapters[i].driverVersion),
                          adapters[i].isIntegrated ? L"integrated" : L"discrete"));
    if (adapters.size() < 2) {
        Print(L"Fewer than two hardware adapters: nothing to test.\n");
        return 0;
    }

    int failures = 0, pairs = 0;
    for (size_t si = 0; si < adapters.size(); ++si) {
        for (size_t di = 0; di < adapters.size(); ++di) {
            if (si == di) continue;
            if (onlySrc >= 0 && (int(si) != onlySrc || int(di) != onlyDst)) continue;
            ++pairs;
            const std::wstring pairName = std::format(L"{} -> {}", adapters[si].name, adapters[di].name);
            Print(std::format(L"\n== [{}] -> [{}] {} ==\n", si, di, pairName));
            std::wstring error;
            auto src = GpuDevice::Create(&adapters[si].luid, error);
            auto dst = src ? GpuDevice::Create(&adapters[di].luid, error) : nullptr;
            if (!src || !dst) {
                Print(std::format(L"  device creation failed: {}\n", error));
                ++failures;
                continue;
            }
            // D3D12 devices are per-adapter singletons: holding them here means the link's D3D12CreateDevice returns
            // these same devices, whose info queues are then read below.
            ComPtr<ID3D12Device> s12, d12;
            if (debugLayers) {
                D3D12CreateDevice(src->Adapter(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s12));
                D3D12CreateDevice(dst->Adapter(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12));
                DumpD3D12Messages(s12.Get(), L"src");
                DumpD3D12Messages(d12.Get(), L"dst");
            }
            for (bool forceCpu : {false, true}) {
                SetForceCpu(forceCpu);
                auto link = CrossAdapterLink::Create(*src, *dst, error);
                SetForceCpu(false);
                if (!link) {
                    Print(std::format(L"  Create failed: {}\n", error));
                    ++failures;
                    continue;
                }
                Print(std::format(L"  method: {}{}\n", link->Method(), forceCpu ? L" (forced)" : L" (automatic)"));
                if (!RunCorrectness(*src, *dst, *link)) ++failures;
                if (debugLayers) {
                    link.reset();  // destruction path included
                    const int n = DumpD3D11Messages(src->Device(), L"src") + DumpD3D11Messages(dst->Device(), L"dst") +
                                  DumpD3D12Messages(s12.Get(), L"src") + DumpD3D12Messages(d12.Get(), L"dst");
                    Print(std::format(L"    debug-layer messages (warning or worse): {}\n", n));
                    if (n) ++failures;
                    continue;
                }
                if (!forceCpu && std::wstring(link->Method()) == L"CPU staging") break; // automatic already = CPU
            }
            if (bench) {
                RunBench(*src, *dst, false, iterations, pairName);
                RunBench(*src, *dst, true, iterations, pairName);
            }
        }
    }
    Print(std::format(L"\n{} pair(s), {} failure(s)\n", pairs, failures));
    return failures == 0 ? 0 : 1;
}
