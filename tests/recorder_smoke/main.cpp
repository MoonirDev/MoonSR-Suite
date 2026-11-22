// Smoke test for src/diag FrameRecorder: records deterministic CPU patterns through the GPU, re-reads the PNGs and
// verifies them pixel-exactly. Single-threaded test driver (the recorder owns its own worker thread).
#include "core/log.h"
#include "diag/frame_recorder.h"
#include "gpu/gpu_device.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace moonsr;
namespace fs = std::filesystem;

namespace {

int g_fail = 0;
void Check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

struct Pixels {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> bgra;  // tightly packed
};

// Deterministic pattern; alpha deliberately not 255 to prove it is ignored/forced.
Pixels MakePattern(int k, uint32_t w, uint32_t h) {
    Pixels p;
    p.w = w;
    p.h = h;
    p.bgra.resize(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* d = &p.bgra[(static_cast<size_t>(y) * w + x) * 4];
            d[0] = static_cast<uint8_t>(x * 3 + k * 17 + y / 2);
            d[1] = static_cast<uint8_t>((y * 5 + k * 31) ^ x);
            d[2] = static_cast<uint8_t>((x ^ y) + k * 64);
            d[3] = 77;
        }
    return p;
}

Pixels CropOf(const Pixels& s, int l, int t, int r, int b) {
    Pixels o;
    o.w = static_cast<uint32_t>(r - l);
    o.h = static_cast<uint32_t>(b - t);
    o.bgra.resize(static_cast<size_t>(o.w) * o.h * 4);
    for (uint32_t y = 0; y < o.h; ++y)
        memcpy(&o.bgra[static_cast<size_t>(y) * o.w * 4], &s.bgra[(static_cast<size_t>(t + y) * s.w + l) * 4], static_cast<size_t>(o.w) * 4);
    return o;
}

ComPtr<ID3D11Texture2D> Upload(ID3D11Device* dev, const Pixels& p, DXGI_FORMAT fmt) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = p.w;
    td.Height = p.h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{p.bgra.data(), p.w * 4, 0};
    ComPtr<ID3D11Texture2D> t;
    const HRESULT hr = dev->CreateTexture2D(&td, &sd, &t);
    if (FAILED(hr)) printf("CreateTexture2D failed 0x%08X\n", static_cast<unsigned>(hr));
    return t;
}

bool ReadPng(IWICImagingFactory* f, const fs::path& path, Pixels& out) {
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    out.w = w;
    out.h = h;
    out.bgra.resize(static_cast<size_t>(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.bgra.size()), out.bgra.data()));
}

bool EqualIgnoringAlpha(const Pixels& a, const Pixels& b) {
    if (a.w != b.w || a.h != b.h) return false;
    for (size_t i = 0; i < a.bgra.size(); i += 4)
        if (a.bgra[i] != b.bgra[i] || a.bgra[i + 1] != b.bgra[i + 1] || a.bgra[i + 2] != b.bgra[i + 2]) return false;
    return true;
}

bool WaitFinished(FrameRecorder& rec, std::wstring& dir) {
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)) {
        if (rec.PollFinished(dir)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::vector<std::string> ReadLines(const fs::path& p) {
    std::ifstream in(p);
    std::vector<std::string> lines;
    std::string l;
    while (std::getline(in, l)) lines.push_back(l);
    return lines;
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> v;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ',')) v.push_back(t);
    return v;
}

struct Expect {
    std::string file;
    std::string kind;
    double phase;
    unsigned long long realIndex;
    Pixels px;
};

} // namespace

int main() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co)) { printf("CoInitializeEx failed\n"); return 1; }
    int rc = 0;
    {
        const fs::path root = fs::path(MOONSR_SOURCE_DIR) / L"benchmarks" / L"output" / L"recorder_smoke";
        std::error_code ec;
        fs::create_directories(root, ec);
        InitLog((root / L"recorder_smoke.log").wstring());
        const fs::path run1 = root / (L"run_" + std::to_wstring(GetTickCount64()));
        const fs::path run2 = root / (L"run_" + std::to_wstring(GetTickCount64()) + L"_b");

        std::wstring err;
        auto gpu = GpuDevice::Create(nullptr, err);
        if (!gpu) { wprintf(L"GpuDevice::Create failed: %s\n", err.c_str()); CoUninitialize(); return 1; }
        wprintf(L"adapter: %s\n", gpu->Info().name.c_str());
        auto rec = FrameRecorder::Create(*gpu, err);
        Check(rec != nullptr, "FrameRecorder::Create");
        if (!rec) { CoUninitialize(); return 1; }

        ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));

        ID3D11Device* dev = gpu->Device();
        const Pixels pA = MakePattern(0, 1283, 717);  // odd width: RowPitch is padded
        const Pixels pB = MakePattern(1, 640, 360);
        const Pixels pC = MakePattern(2, 1283, 717);
        const Pixels pD = MakePattern(3, 321, 199);
        const Pixels pE = MakePattern(4, 800, 450);
        auto tA = Upload(dev, pA, DXGI_FORMAT_B8G8R8A8_UNORM);
        auto tB = Upload(dev, pB, DXGI_FORMAT_B8G8R8A8_UNORM);
        auto tC = Upload(dev, pC, DXGI_FORMAT_B8G8R8A8_TYPELESS);
        auto tD = Upload(dev, pD, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        auto tE = Upload(dev, pE, DXGI_FORMAT_B8G8R8A8_UNORM);
        Check(tA && tB && tC && tD && tE, "source textures created");
        if (!(tA && tB && tC && tD && tE)) { CoUninitialize(); return 1; }

        // ---- recording 1 ----
        Check(!rec->Active(), "Active() false before Begin");
        {
            // Capture before Begin is a no-op.
            RecordedFrameInfo i;
            rec->Capture(tA.Get(), RectI{}, i);
        }
        Check(rec->Begin(run1.wstring(), 6, L"recorder_smoke run 1"), "Begin recording 1 (6 outputs)");
        Check(rec->Active(), "Active() true after Begin");
        Check(!rec->Begin((run1 / L"other").wstring(), 3, L"x"), "second Begin while active is rejected");

        std::vector<Expect> exp;  // in capture order
        auto cap = [&](ID3D11Texture2D* t, const RectI& region, RecordedKind kind, float phase, uint64_t realIndex, const std::string& file,
                       const std::string& kindStr, Pixels expected) {
            RecordedFrameInfo info;
            info.kind = kind;
            info.phase = phase;
            info.dueQpc = 1000 + static_cast<int64_t>(exp.size());
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            info.actualQpc = q.QuadPart;
            info.realIndex = realIndex;
            rec->Capture(t, region, info);
            exp.push_back({file, kindStr, phase, realIndex, std::move(expected)});
        };

        bool activeUntilLast = true;
        cap(tA.Get(), RectI{}, RecordedKind::Input, 1.0f, 1, "in_0001.png", "input", pA);
        cap(tA.Get(), RectI{}, RecordedKind::Real, 1.0f, 1, "out_0001_real.png", "real", pA);
        activeUntilLast &= rec->Active();
        cap(tB.Get(), RectI{}, RecordedKind::Input, 1.0f, 2, "in_0002.png", "input", pB);
        cap(tB.Get(), RectI{}, RecordedKind::Generated, 0.5f, 2, "out_0002_gen_t0.50.png", "generated", pB);
        activeUntilLast &= rec->Active();
        cap(tC.Get(), RectI{}, RecordedKind::Real, 1.0f, 2, "out_0003_real.png", "real", pC);
        activeUntilLast &= rec->Active();
        cap(tC.Get(), RectI{}, RecordedKind::Input, 1.0f, 3, "in_0003.png", "input", pC);
        // Oversized region is clamped to the whole texture (typed sRGB source).
        cap(tD.Get(), RectI{-10, -10, 5000, 5000}, RecordedKind::Generated, 0.25f, 3, "out_0004_gen_t0.25.png", "generated", pD);
        activeUntilLast &= rec->Active();
        // Sub-region with odd offsets from the large odd-width texture.
        cap(tA.Get(), RectI{101, 33, 1000, 600}, RecordedKind::Generated, 0.75f, 3, "out_0005_gen_t0.75.png", "generated",
            CropOf(pA, 101, 33, 1000, 600));
        activeUntilLast &= rec->Active();
        Check(activeUntilLast, "Active() stays true until the 6th output");
        cap(tE.Get(), RectI{}, RecordedKind::Real, 1.0f, 4, "out_0006_real.png", "real", pE);
        Check(!rec->Active(), "Active() false right after the 6th output");

        // Inputs and outputs after completion are ignored.
        {
            RecordedFrameInfo i;
            i.kind = RecordedKind::Input;
            i.realIndex = 4;
            rec->Capture(tA.Get(), RectI{}, i);
            i.kind = RecordedKind::Real;
            rec->Capture(tA.Get(), RectI{}, i);
        }

        std::wstring finishedDir;
        const bool fin = WaitFinished(*rec, finishedDir);
        Check(fin, "PollFinished becomes true");
        Check(fin && fs::path(finishedDir) == run1, "PollFinished reports the recording directory");
        std::wstring again;
        Check(!rec->PollFinished(again), "PollFinished returns true exactly once");

        // Files present: exactly 9 images + index.csv + info.txt.
        std::set<std::string> found;
        for (const auto& e : fs::directory_iterator(run1)) found.insert(e.path().filename().string());
        std::set<std::string> want{"index.csv", "info.txt"};
        for (const auto& e : exp) want.insert(e.file);
        Check(found == want, "directory holds exactly the expected files (no extra in_/out_ after completion)");
        if (found != want) {
            for (const auto& f : found) printf("  found: %s\n", f.c_str());
            for (const auto& f : want) printf("  want:  %s\n", f.c_str());
        }

        // Pixel-exact verification.
        bool allPixels = true;
        for (const auto& e : exp) {
            Pixels got;
            const bool ok = ReadPng(wic.Get(), run1 / e.file, got) && EqualIgnoringAlpha(got, e.px);
            printf("  %s %s (%ux%u)\n", ok ? "ok  " : "BAD ", e.file.c_str(), got.w, got.h);
            allPixels &= ok;
        }
        Check(allPixels, "all PNGs pixel-exact against source patterns");

        // index.csv
        {
            const auto lines = ReadLines(run1 / L"index.csv");
            bool ok = lines.size() == exp.size() + 1;
            Check(ok, "index.csv has header + one row per image");
            bool rowsOk = ok;
            for (size_t i = 0; ok && i < exp.size(); ++i) {
                const auto c = Split(lines[i + 1]);
                const auto& e = exp[i];
                const bool r = c.size() == 9 && c[0] == e.file && c[1] == e.kind && std::abs(atof(c[2].c_str()) - e.phase) < 1e-3 &&
                               strtoull(c[3].c_str(), nullptr, 10) == e.realIndex && strtoll(c[4].c_str(), nullptr, 10) == 1000 + static_cast<long long>(i) &&
                               atof(c[6].c_str()) >= 0.0 && strtoul(c[7].c_str(), nullptr, 10) == e.px.w && strtoul(c[8].c_str(), nullptr, 10) == e.px.h;
                if (!r) printf("  bad row %zu: %s\n", i, lines[i + 1].c_str());
                rowsOk &= r;
            }
            Check(rowsOk, "index.csv rows match file, kind, phase, realIndex, dueQpc, size");
            if (lines.size() > 1) printf("  header: %s\n  row1:   %s\n", lines[0].c_str(), lines[1].c_str());
            const auto info = ReadLines(run1 / L"info.txt");
            std::string all;
            for (const auto& l : info) all += l + "\n";
            Check(all.find("recorder_smoke run 1") != std::string::npos && all.find("input_frames: 3") != std::string::npos &&
                      all.find("output_frames: 6") != std::string::npos && all.find("qpc_frequency:") != std::string::npos,
                  "info.txt has description, counts and QPC frequency");
        }

        // ---- recording 2 (second Begin after the first finished; also checks clamping 0 -> 1 frame) ----
        Check(rec->Begin(run2.wstring(), 0, L"recorder_smoke run 2"), "second recording Begin after first finished (0 clamps to 1)");
        {
            RecordedFrameInfo i;
            i.kind = RecordedKind::Generated;
            i.phase = 0.33f;
            i.realIndex = 9;
            rec->Capture(tB.Get(), RectI{}, i);
        }
        Check(!rec->Active(), "Active() false after the single requested output");
        Check(WaitFinished(*rec, finishedDir) && fs::path(finishedDir) == run2, "second recording finishes");
        Pixels got2;
        Check(ReadPng(wic.Get(), run2 / L"out_0001_gen_t0.33.png", got2) && EqualIgnoringAlpha(got2, pB), "second recording PNG pixel-exact");
        // Counters restarted: file numbering began at 0001 again.
        Check(fs::exists(run2 / L"index.csv"), "second recording wrote index.csv");

        // Capture after completion is a no-op.
        {
            RecordedFrameInfo i;
            i.kind = RecordedKind::Real;
            rec->Capture(tB.Get(), RectI{}, i);
        }
        size_t count2 = 0;
        for (const auto& e : fs::directory_iterator(run2)) { (void)e; ++count2; }
        Check(count2 == 3, "Capture after completion writes nothing (1 png + index.csv + info.txt)");
        Check(!rec->PollFinished(again), "no spurious PollFinished");

        // Destructor drains and joins.
        rec.reset();
        Check(true, "recorder destroyed (worker joined)");
        printf("output folder: %ls\n", run1.c_str());
        rc = g_fail == 0 ? 0 : 1;
    }
    CoUninitialize();
    printf(rc == 0 ? "ALL CHECKS PASSED\n" : "%d CHECK(S) FAILED\n", g_fail);
    return rc;
}
