// moonsr_fgbench: offline frame generation benchmark. Runs the real MoonSR FrameGenerator on a chosen GPU against
// synthetic sequences with exact (supersampled, CPU-rendered) ground truth, compares with CPU baselines, and times
// PushFrame / Interpolate with D3D11 timestamp queries. Results are synthetic-content measurements only.
#include "fgbench.h"

#include "core/log.h"
#include "framegen/adaptive_policy.h"
#include "framegen/adaptive_profile.h"
#include "framegen/adaptive_sampler.h"
#include "framegen/adaptive_controller.h"
#include "engine/frame_pacing.h"

#include <windows.h>

#include <chrono>
#include <array>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;
using namespace fgb;
using moonsr::FrameGenDebugView;
using moonsr::FrameGenQuality;
using moonsr::FrameGenSettings;

namespace {

struct Config {
    FrameGenQuality q;
    float flowScale;
};

struct Options {
    bool selftest = false, quality = false, time = false, list = false, saveImages = false, allAdapters = false, help = false;
    bool preciseMotion = false;
    std::vector<std::string> adapters, seqFilter;
    uint32_t w = 1280, h = 720;
    uint32_t history = 1;
    std::vector<Config> qualityConfigs{{FrameGenQuality::Performance, 0.5f}, {FrameGenQuality::Balanced, 0.5f},
                                       {FrameGenQuality::Quality, 0.5f},     {FrameGenQuality::Balanced, 0.25f},
                                       {FrameGenQuality::Balanced, 1.0f}};
    std::vector<FrameGenQuality> timeTiers{FrameGenQuality::Performance, FrameGenQuality::Balanced, FrameGenQuality::Quality};
    std::vector<float> timeScales{0.25f, 0.5f, 1.0f};
    std::vector<std::pair<uint32_t, uint32_t>> timeSizes{{1280, 720}, {1920, 1080}};
    uint32_t iterations = 200, warmup = 30;
    fs::path outDir;
    std::string tag;
    std::string source;            // real frame used as texture for the real_* sequences (and as timing content)
    std::vector<std::string> pair; // real captured pair: [before,] A, B
    std::vector<std::string> triplet; // equally spaced captured frames A, truth, C
};

const char* TierName(FrameGenQuality q) {
    switch (q) {
    case FrameGenQuality::Performance: return "performance";
    case FrameGenQuality::Quality: return "quality";
    default: return "balanced";
    }
}

bool ParseTier(const std::string& s, FrameGenQuality& q) {
    if (s.rfind("p", 0) == 0) q = FrameGenQuality::Performance;
    else if (s.rfind("b", 0) == 0) q = FrameGenQuality::Balanced;
    else if (s.rfind("q", 0) == 0) q = FrameGenQuality::Quality;
    else return false;
    return true;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep))
        if (!item.empty()) out.push_back(item);
    return out;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Safe(std::string s) {
    for (auto& c : s)
        if (!isalnum(static_cast<unsigned char>(c))) c = '_';
    return s;
}

std::string Num(double v, int prec = 3) {
    if (std::isinf(v)) return "inf";
    char b[32];
    snprintf(b, sizeof(b), "%.*f", prec, v);
    return b;
}

void Help() {
    printf(R"(moonsr_fgbench - frame generation benchmark (real FrameGenerator on a real GPU, synthetic ground truth)

Modes:
  --selftest            harness and contract checks (exit code 1 on failure)
  --quality             synthetic sequences: PSNR / SSIM of MoonSR FG vs repeat-nearest and linear-blend baselines
  --time                GPU timestamp timing of PushFrame and Interpolate(0.5), isolated, idle GPU
  --list-adapters
  --pair [P,]A,B        real captured pair (PNG): generated frames at t = 0.25 / 0.5 / 0.75, debug views and crop
                        mosaics (A | generated | B) under <out>/fg_pair/<tag>/<config>/ for each --configs entry
  --triplet A,B,C       compare interpolation of real frames A and C with held-out real middle frame B (PNG)
Selection:
  --source <png>        real captured frame: adds the real_* sequences (real image content, exactly known synthetic
                        motion) to --quality, and is used as --time content
  --adapter <substr>    adapter name substring (repeatable); default: first hardware adapter
  --all-adapters        every hardware adapter
  --sequences a,b       only these sequences (substring match)
  --size WxH            quality sequence size (default 1280x720)
  --configs t:s,...     quality configs, tier p|b|q and flow scale (default p:0.5,b:0.5,q:0.5,b:0.25,b:1)
  --tiers p,b,q         timing tiers          --flow-scales 0.25,0.5,1   timing flow scales
  --time-sizes WxH,...  timing sizes (default 1280x720,1920x1080)
  --iterations N        timed iterations (default 200)   --warmup N (default 30)
  --history N           real frames preceding A in quality tests (default 1, range 0..16)
  --precise-motion      use subpixel motion refinement in quality and timing tests
  --save-images         PNGs under <out>/fg/<adapter>/<config>/<sequence>/
  --out-dir <dir>       default <repo>/benchmarks/output     --tag <name> result file tag (default timestamp)
)");
}

bool Parse(int argc, char** argv, Options& o, std::string& err) {
    auto need = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            err = std::string("missing value for ") + argv[i];
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = nullptr;
        if (a == "--selftest") o.selftest = true;
        else if (a == "--quality") o.quality = true;
        else if (a == "--time") o.time = true;
        else if (a == "--list-adapters") o.list = true;
        else if (a == "--save-images") o.saveImages = true;
        else if (a == "--precise-motion") o.preciseMotion = true;
        else if (a == "--all-adapters") o.allAdapters = true;
        else if (a == "--help" || a == "-h") o.help = true;
        else if (a == "--adapter") { if (!(v = need(i))) return false; o.adapters.push_back(Lower(v)); }
        else if (a == "--sequences") { if (!(v = need(i))) return false; o.seqFilter = Split(v, ','); }
        else if (a == "--size") {
            if (!(v = need(i))) return false;
            if (sscanf_s(v, "%ux%u", &o.w, &o.h) != 2 || o.w < 64 || o.h < 64) { err = "bad --size"; return false; }
        } else if (a == "--configs") {
            if (!(v = need(i))) return false;
            o.qualityConfigs.clear();
            for (auto& c : Split(v, ',')) {
                const auto p = Split(c, ':');
                Config cfg{};
                if (p.size() != 2 || !ParseTier(p[0], cfg.q)) { err = "bad --configs"; return false; }
                cfg.flowScale = static_cast<float>(atof(p[1].c_str()));
                o.qualityConfigs.push_back(cfg);
            }
        } else if (a == "--tiers") {
            if (!(v = need(i))) return false;
            o.timeTiers.clear();
            for (auto& t : Split(v, ',')) {
                FrameGenQuality q;
                if (!ParseTier(t, q)) { err = "bad --tiers"; return false; }
                o.timeTiers.push_back(q);
            }
        } else if (a == "--flow-scales") {
            if (!(v = need(i))) return false;
            o.timeScales.clear();
            for (auto& t : Split(v, ',')) o.timeScales.push_back(static_cast<float>(atof(t.c_str())));
        } else if (a == "--time-sizes") {
            if (!(v = need(i))) return false;
            o.timeSizes.clear();
            for (auto& t : Split(v, ',')) {
                uint32_t w = 0, h = 0;
                if (sscanf_s(t.c_str(), "%ux%u", &w, &h) != 2) { err = "bad --time-sizes"; return false; }
                o.timeSizes.push_back({w, h});
            }
        } else if (a == "--history") { if (!(v = need(i))) return false; o.history = static_cast<uint32_t>(std::clamp(atoi(v), 0, 16)); }
        else if (a == "--iterations") { if (!(v = need(i))) return false; o.iterations = std::max(1, atoi(v)); }
        else if (a == "--warmup") { if (!(v = need(i))) return false; o.warmup = std::max(0, atoi(v)); }
        else if (a == "--out-dir") { if (!(v = need(i))) return false; o.outDir = v; }
        else if (a == "--tag") { if (!(v = need(i))) return false; o.tag = v; }
        else if (a == "--source") { if (!(v = need(i))) return false; o.source = v; }
        else if (a == "--pair") {
            if (!(v = need(i))) return false;
            o.pair = Split(v, ',');
            if (o.pair.size() < 2 || o.pair.size() > 3) { err = "bad --pair (2 or 3 PNG paths)"; return false; }
        }
        else if (a == "--triplet") {
            if (!(v = need(i))) return false;
            o.triplet = Split(v, ',');
            if (o.triplet.size() != 3) { err = "bad --triplet (exactly 3 PNG paths)"; return false; }
        }
        else { err = "unknown argument " + a; return false; }
    }
    return true;
}

fs::path RepoRoot() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path p = fs::path(buf).parent_path();
    for (int i = 0; i < 8 && !p.empty(); ++i) {
        if (fs::exists(p / "CMakeLists.txt") && fs::exists(p / "shaders")) return p;
        p = p.parent_path();
    }
    return fs::current_path();
}

std::vector<moonsr::AdapterInfo> SelectAdapters(const Options& o) {
    std::vector<moonsr::AdapterInfo> all = moonsr::GpuDevice::EnumerateAdapters(), out;
    for (const auto& a : all) {
        if (a.isSoftware || a.maxFeatureLevel < D3D_FEATURE_LEVEL_11_0) continue;
        const std::string name = Lower(Narrow(a.name));
        if (o.allAdapters) out.push_back(a);
        else if (!o.adapters.empty()) {
            for (const auto& f : o.adapters)
                if (name.find(f) != std::string::npos) {
                    out.push_back(a);
                    break;
                }
        } else if (out.empty()) out.push_back(a);
    }
    return out;
}

void Save(const fs::path& dir, const char* name, const Image& img) {
    std::string err;
    if (!WritePng((dir / name).wstring(), img, err)) printf("  ! %s: %s\n", name, err.c_str());
}

Image AbsDiff4(const Image& a, const Image& b) {
    Image o;
    o.Alloc(a.w, a.h);
    for (size_t i = 0; i < o.px.size(); ++i)
        if ((i & 3) != 3) o.px[i] = static_cast<uint8_t>(std::min(255, 4 * std::abs(static_cast<int>(a.px[i]) - b.px[i])));
    return o;
}

// Side-by-side zoomed crops (nearest, `zoom` x) of the same rectangle from several images, 4 px gaps.
Image Mosaic(const std::vector<const Image*>& imgs, int x0, int y0, int cw, int ch, int zoom) {
    Image o;
    const int n = static_cast<int>(imgs.size());
    o.Alloc(static_cast<uint32_t>(n * cw * zoom + (n - 1) * 4), static_cast<uint32_t>(ch * zoom));
    std::fill(o.px.begin(), o.px.end(), static_cast<uint8_t>(255));
    for (int k = 0; k < n; ++k) {
        const Image& s = *imgs[k];
        for (int y = 0; y < ch * zoom; ++y)
            for (int x = 0; x < cw * zoom; ++x) {
                const int sx = std::clamp(x0 + x / zoom, 0, static_cast<int>(s.w) - 1);
                const int sy = std::clamp(y0 + y / zoom, 0, static_cast<int>(s.h) - 1);
                const uint8_t* p = &s.px[(static_cast<size_t>(sy) * s.w + sx) * 4];
                uint8_t* q = &o.px[(static_cast<size_t>(y) * o.w + k * (cw * zoom + 4) + x) * 4];
                q[0] = p[0];
                q[1] = p[1];
                q[2] = p[2];
            }
    }
    return o;
}

struct Crop {
    const char* name;
    int x, y, w, h;
};

// Regions worth inspecting per sequence (positions follow synth.cpp).
std::vector<Crop> CropsFor(const std::string& seq, uint32_t w, uint32_t h) {
    const int W = static_cast<int>(w), H = static_cast<int>(h);
    if (seq == "hud_pan")
        return {{"text", 24, 20, 150, 70}, {"crosshair", W / 2 - 40, H / 2 - 40, 80, 80}, {"bar", 30, H - 70, 150, 40},
                {"panel", W - 280, 20, 150, 70}};
    if (seq == "real_layers" || seq == "real_layers_aa")
        return {{"sprite_edge", 500, 330, 150, 100}, {"pillar", 440, 110, 150, 100}, {"hud_quest", 960, 60, 150, 100},
                {"minimap", 110, 90, 150, 100}};
    if (seq == "real_rotate" || seq == "real_rotate_aa") return {{"far", 150, 120, 150, 100}, {"centre", W / 2 - 75, H / 2 - 50, 150, 100}};
    if (seq == "real_turn") return {{"stairs", 760, 250, 150, 100}, {"centre", W / 2 - 75, H / 2 - 50, 150, 100}};
    if (seq == "small_fast_object") return {{"object", static_cast<int>(0.3 * W) - 30, static_cast<int>(0.4 * H) - 50, 150, 100}};
    if (seq == "layers_occlusion") return {{"edge", static_cast<int>(0.38 * W) - 75, static_cast<int>(0.4 * H) - 50, 150, 100}};
    return {{"centre", W / 2 - 75, H / 2 - 50, 150, 100}};
}

// ---- data for one sequence (rendered once, shared across adapters / configs) ----
struct SeqData {
    Sequence seq;
    std::vector<Image> history; // real frames before A, for exercising temporal motion tracking
    Image a, b;
    std::vector<double> ts;
    std::vector<Image> gt;
    Mask hud;
    bool hasHud = false;
};

const std::vector<double> kTs{0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75};

std::string TName(double t) {
    char b[16];
    snprintf(b, sizeof(b), "t%03d", static_cast<int>(std::lround(t * 1000)));
    return b;
}

// ---- self-test ----
int SelfTest(Runner* gpu) {
    int fails = 0;
    auto check = [&](bool ok, const std::string& what) {
        printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
        if (!ok) ++fails;
    };
    printf("Self-test (harness):\n");
    check(moonsr::StartingFrameMultiplier(3, 1) == 2 && moonsr::StartingFrameMultiplier(6, 1) == 2 &&
          moonsr::StartingFrameMultiplier(4, 3) == 3 && moonsr::StartingFrameMultiplier(1, 3) == 1,
          "higher frame-generation multipliers start conservatively");
    check(moonsr::ShouldSkipStaleGenerated(true, false, 7.0, 11.0, 1.5) &&
          !moonsr::ShouldSkipStaleGenerated(true, true, 7.0, 100.0, 1.5) &&
          !moonsr::ShouldSkipStaleGenerated(false, false, 7.0, 100.0, 1.5) &&
          !moonsr::ShouldSkipStaleGenerated(true, false, 7.0, 10.5, 1.5) &&
          !moonsr::ShouldSkipStaleGenerated(true, false, 7.0, 100.0, 0.0),
          "catch-up skips only stale generated V-Sync frames");
    const Image n1 = NoiseImage(320, 180, 5);
    const Quality qi = Compare(n1, n1, 0, nullptr);
    check(std::isinf(qi.psnr) && std::abs(qi.ssim - 1.0) < 1e-9, "identical images: PSNR inf, SSIM 1");
    Image n2 = n1;
    for (size_t i = 0; i < n2.px.size(); ++i)
        if ((i & 3) != 3) n2.px[i] = static_cast<uint8_t>(std::min(255, n2.px[i] + 1));
    {
        // Saturated values cannot be +1; compute the expected MSE exactly.
        double se = 0;
        for (size_t i = 0; i < n1.px.size(); ++i)
            if ((i & 3) != 3) se += (n2.px[i] - n1.px[i]) * (n2.px[i] - n1.px[i]);
        const double expect = 10 * std::log10(255.0 * 255.0 / (se / (3.0 * n1.w * n1.h)));
        check(std::abs(Compare(n1, n2, 0, nullptr).psnr - expect) < 1e-9, "PSNR of a +1 offset matches the closed form");
    }
    {
        // Ground-truth renderer: an integer pan must give a pixel-exact shifted image.
        auto seqs = MakeSequences(256, 144);
        Sequence pan = seqs[0];
        const auto base = seqs[1].scene;
        pan.scene = [base](double x, double y, double t) { return base(x + 7 * t, y + 3 * t, 0); };
        const Image a = Render(pan, 256, 144, 0.0), b = Render(pan, 256, 144, 1.0);
        double m = 0;
        for (uint32_t y = 0; y + 3 < 144; ++y)
            for (uint32_t x = 0; x + 7 < 256; ++x)
                for (int c = 0; c < 3; ++c)
                    m = std::max(m, std::abs(static_cast<double>(b.px[(y * 256 + x) * 4 + c]) - a.px[((y + 3) * 256 + x + 7) * 4 + c]));
        check(m == 0, "renderer: integer pan of the continuous scene is pixel exact");
    }
    const Image bl = LinearBlend(n1, n2, 0.5);
    check(MaxAbsDiff(bl, n1) <= 1, "linear blend baseline");
    {
        bool covered[25]{};
        bool bounded = true;
        for (uint32_t i = 0; i < 108; ++i) {
            const auto [x, y] = moonsr::fgadaptive::SampleTileOffset(i, 1760, 920);
            bounded = bounded && x <= 1760 && y <= 920 &&
                      moonsr::fgadaptive::SampleTileOffset(i, 1760, 920) == std::pair{x, y};
            covered[std::min(4u, x * 5 / 1761) + 5 * std::min(4u, y * 5 / 921)] = true;
        }
        int regions = 0;
        for (bool seen : covered) regions += seen ? 1 : 0;
        check(bounded && regions >= 24 && moonsr::fgadaptive::SampleTileOffset(8, 1760, 920) == std::pair{1760u, 920u} &&
              moonsr::fgadaptive::SampleTileOffset(27, 0, 0) == std::pair{0u, 0u},
              "adaptive sampling explores beyond the original grid without leaving the capture");
    }
    {
        moonsr::fgadaptive::Evidence evidence;
        for (uint64_t i = 1; i <= 96; ++i) evidence.Add({i, 10.0, 10.1, 2.0, 1.5, (i & 1) != 0});
        check(evidence.Status().accepted, "adaptive evidence accepts a validated faster candidate");
        {
            moonsr::fgadaptive::Evidence recent = evidence;
            for (uint64_t i = 1001; i <= 1003; ++i) recent.Add({i, 10.0, 18.0, 2.0, 1.5, (i & 1) != 0});
            check(recent.Status().recentRegression && !recent.Status().accepted,
                  "adaptive evidence rejects repeated recent quality regressions");
            for (uint64_t i = 1004; i <= 1007; ++i) recent.Add({i, 10.0, 10.1, 2.0, 1.5, (i & 1) != 0});
            check(!recent.Status().recentRegression, "adaptive recent-regression guard clears after new evidence");
            moonsr::fgadaptive::Evidence noise = evidence;
            for (uint64_t i = 2001; i <= 2003; ++i) noise.Add({i, 0.0, 0.5, 2.0, 1.5, (i & 1) != 0});
            check(!noise.Status().recentRegression, "adaptive guard ignores tiny absolute reconstruction differences");
        }
        check(!evidence.Add({1, 10, 10, 2, 1}), "adaptive evidence ignores duplicate samples");
        check(!evidence.Add({999, -1, 1, 2, 1}), "adaptive evidence rejects invalid metrics");
        evidence.Reset();
        for (uint64_t i = 1; i <= 96; ++i) evidence.Add({i, 10.0, 12.0, 2.0, 1.5, (i & 1) != 0});
        check(!evidence.Status().accepted, "adaptive evidence rejects a quality regression");
        evidence.Reset();
        for (uint64_t i = 1; i <= 96; ++i) evidence.Add({i, 10.0, 10.0, 2.0, 1.95, (i & 1) != 0});
        check(!evidence.Status().accepted, "adaptive evidence rejects a candidate with insufficient speed gain");
        evidence.Reset();
        for (uint64_t i = 1; i <= 96; ++i)
            evidence.Add({i, 10.0, 10.0, i % 4 == 0 ? 20.0 : 2.0, i % 4 == 0 ? 1.5 : 2.2, (i & 1) != 0});
        check(!evidence.Status().accepted, "adaptive evidence rejects an apparent gain caused by timing outliers");
        wchar_t tmpPath[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, tmpPath, MAX_PATH);
        if (n && n < MAX_PATH) {
            const std::wstring name = L"moonsr-fg-profile-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                      std::to_wstring(GetTickCount64()) + L".profile";
            const std::wstring path = (fs::path(tmpPath).parent_path() / name).wstring();
            moonsr::fgadaptive::ProfileContext context{L"game.exe", L"test gpu", 1, 2, 1920, 1080, 100, true};
            auto preciseContext = context;
            preciseContext.preciseMotion = true;
            check(moonsr::fgadaptive::ProfileKey(context) != moonsr::fgadaptive::ProfileKey(preciseContext),
                  "adaptive profiles separate precise-motion evidence");
            moonsr::fgadaptive::Evidence loaded;
            const bool saved = moonsr::fgadaptive::SaveProfile(path, context, evidence);
            const bool loadedOk = saved && moonsr::fgadaptive::LoadProfile(path, context, loaded);
            check(loadedOk && loaded.Samples().size() == evidence.Samples().size() &&
                  loaded.Samples()[0].candidateFirst == evidence.Samples()[0].candidateFirst,
                  "adaptive profile survives save and reload, including measurement order");
            auto other = context;
            other.width = 1280;
            check(!moonsr::fgadaptive::LoadProfile(path, other, loaded), "adaptive profile rejects a different resolution");
            other = context;
            other.driverVersion = 42;
            check(!moonsr::fgadaptive::LoadProfile(path, other, loaded), "adaptive profile rejects a different GPU driver");
            {
                std::ofstream corrupt{fs::path(path), std::ios::trunc};
                corrupt << "MOONSR_FG 5 " << moonsr::fgadaptive::ProfileKey(context) << " 129\n";
            }
            check(!moonsr::fgadaptive::LoadProfile(path, context, loaded) && loaded.Samples().size() == evidence.Samples().size(),
                  "corrupt adaptive profile is rejected without changing current evidence");
            DeleteFileW(path.c_str());
        } else {
            check(false, "adaptive profile test could not find the executable directory");
        }
    }
    if (!gpu) {
        printf("No GPU: skipping generator checks.\n");
        return fails;
    }

    printf("Self-test (FrameGenerator on %s):\n", Narrow(gpu->Info().name).c_str());
    std::string err;
    {
        wchar_t exePath[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        bool ok = n && n < MAX_PATH;
        const fs::path directory = fs::path(exePath).parent_path() /
            (L"fg-controller-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        moonsr::fgadaptive::ProfileContext context{L"test-game.exe", gpu->Info().name, gpu->Info().vendorId,
                                                   gpu->Info().deviceId, 1920, 1080, 100, true};
        context.gameIdentity = L"c:\\games\\test-game.exe";
        context.candidateFlowPercent = 75;
        moonsr::fgadaptive::Evidence evidence;
        for (uint64_t i = 1; i <= 96; ++i) evidence.Add({i, 10.0, 10.0, 2.0, 1.5, (i & 1) != 0});
        const std::wstring path = moonsr::fgadaptive::ProfilePath(directory.wstring(), context);
        const std::wstring legacyPath = moonsr::fgadaptive::LegacyProfilePath(directory.wstring(), context);
        ok = ok && moonsr::fgadaptive::SaveProfile(legacyPath, context, evidence);
        auto otherGame = context;
        otherGame.game = L"another-game.exe";
        otherGame.gameIdentity = L"d:\\games\\test-game.exe";
        const std::wstring otherPath = moonsr::fgadaptive::ProfilePath(directory.wstring(), otherGame);
        ok = ok && moonsr::fgadaptive::SaveProfile(otherPath, otherGame, evidence);
        wchar_t originalFullMode[16]{};
        const DWORD originalFullLength = GetEnvironmentVariableW(L"MOONSR_FG_FULL_CONTEXT", originalFullMode,
                                                                 static_cast<DWORD>(std::size(originalFullMode)));
        SetEnvironmentVariableW(L"MOONSR_FG_FULL_CONTEXT", L"0");
        {
            wchar_t savedFullMode[16]{};
            const DWORD savedLength = GetEnvironmentVariableW(L"MOONSR_FG_FULL_CONTEXT", savedFullMode,
                                                              static_cast<DWORD>(std::size(savedFullMode)));
            SetEnvironmentVariableW(L"MOONSR_FG_FULL_CONTEXT", L"1");
            {
                moonsr::fgadaptive::Controller controller(gpu->Device());
                FrameGenSettings baseline; baseline.flowScale = 1.0f;
                auto userContext = context; userContext.candidateFlowPercent = 0;
                controller.Configure(true, userContext, baseline, directory.wstring());
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!controller.Status().collecting && std::chrono::steady_clock::now() < deadline) {
                    controller.Tick(true); Sleep(1);
                }
                check(controller.Status().collecting && !controller.Status().profileLoaded &&
                      !controller.Status().accepted && controller.EffectiveSettings().flowScale == 1.0f,
                      "full-context learning does not reuse tile-only profiles");
            }
            auto fullProfile = context;
            fullProfile.game += L"|full-context-v1";
            const std::wstring fullPath = moonsr::fgadaptive::ProfilePath(directory.wstring(), fullProfile);
            bool fullOk = moonsr::fgadaptive::SaveProfile(fullPath, fullProfile, evidence);
            {
                moonsr::fgadaptive::Controller controller(gpu->Device());
                FrameGenSettings baseline; baseline.flowScale = 1.0f;
                auto userContext = context; userContext.candidateFlowPercent = 0;
                controller.Configure(true, userContext, baseline, directory.wstring());
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!controller.Status().profileLoaded && std::chrono::steady_clock::now() < deadline) {
                    controller.Tick(true); Sleep(1);
                }
                fullOk = fullOk && controller.Status().profileLoaded && controller.Status().accepted &&
                         std::abs(controller.EffectiveSettings().flowScale - 0.75f) < 0.001f;
                controller.ResetLearning();
                const auto resetDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (fs::exists(fullPath) && std::chrono::steady_clock::now() < resetDeadline) Sleep(1);
                fullOk = fullOk && !fs::exists(fullPath) && fs::exists(legacyPath);
            }
            check(fullOk, "full-context learning loads, applies and resets its own profile");
            SetEnvironmentVariableW(L"MOONSR_FG_FULL_CONTEXT",
                                    savedLength > 0 && savedLength < std::size(savedFullMode) ? savedFullMode : nullptr);
        }
        {
            moonsr::fgadaptive::Controller controller(gpu->Device());
            FrameGenSettings baseline;
            baseline.flowScale = 1.0f;
            auto userContext = context;
            userContext.candidateFlowPercent = 0;
            controller.Configure(true, userContext, baseline, directory.wstring());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!controller.Status().profileLoaded && std::chrono::steady_clock::now() < deadline) {
                controller.Tick(true); Sleep(1);
            }
            ok = ok && controller.Status().profileLoaded && controller.Status().accepted &&
                 std::abs(controller.EffectiveSettings().flowScale - 0.75f) < 0.001f;
            ok = ok && fs::exists(path) && !fs::exists(legacyPath) &&
                 fs::path(path).parent_path() != fs::path(otherPath).parent_path();
            controller.Configure(false, userContext, baseline, directory.wstring());
            controller.Tick(true);
            ok = ok && !controller.Status().enabled && controller.EffectiveSettings().flowScale == 1.0f;
            controller.Configure(true, userContext, baseline, directory.wstring());
            const auto reloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!controller.Status().profileLoaded && std::chrono::steady_clock::now() < reloadDeadline) {
                controller.Tick(true); Sleep(1);
            }
            ok = ok && controller.Status().profileLoaded && controller.Status().accepted;
            controller.ResetLearning();
            ok = ok && !controller.Status().accepted && controller.EffectiveSettings().flowScale == 1.0f;
            const auto resetDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (fs::exists(path) && std::chrono::steady_clock::now() < resetDeadline) Sleep(1);
            ok = ok && !fs::exists(path) && fs::exists(otherPath);
            controller.Configure(false, userContext, baseline, directory.wstring());
            controller.Tick(true);
            ok = ok && !controller.Status().enabled && controller.EffectiveSettings().flowScale == 1.0f;
        }
        for (const bool tryHalfResolution : {false, true}) {
            moonsr::fgadaptive::Evidence rejected;
            for (uint64_t i = 1; i <= 96; ++i) rejected.Add({i, 10.0, 15.0, 2.0, 1.5, (i & 1) != 0});
            auto gentler = context;
            gentler.candidateFlowPercent = 90;
            const std::wstring gentlerPath = moonsr::fgadaptive::ProfilePath(directory.wstring(), gentler);
            auto half = context;
            half.candidateFlowPercent = 50;
            const std::wstring halfPath = moonsr::fgadaptive::ProfilePath(directory.wstring(), half);
            ok = ok && moonsr::fgadaptive::SaveProfile(path, context, rejected) &&
                 moonsr::fgadaptive::SaveProfile(gentlerPath, gentler, tryHalfResolution ? rejected : evidence);
            if (tryHalfResolution) ok = ok && moonsr::fgadaptive::SaveProfile(halfPath, half, evidence);
            moonsr::fgadaptive::Controller controller(gpu->Device());
            FrameGenSettings baseline; baseline.flowScale = 1.0f;
            auto userContext = context; userContext.candidateFlowPercent = 0;
            controller.Configure(true, userContext, baseline, directory.wstring());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            const float expectedFlow = tryHalfResolution ? 0.5f : 0.9f;
            while ((!controller.Status().accepted || std::abs(controller.EffectiveSettings().flowScale - expectedFlow) >= 0.001f) &&
                   std::chrono::steady_clock::now() < deadline) { controller.Tick(true); Sleep(1); }
            ok = ok && controller.Status().accepted && std::abs(controller.EffectiveSettings().flowScale - expectedFlow) < 0.001f;
            controller.ResetLearning();
            const auto resetDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while ((fs::exists(path) || fs::exists(gentlerPath) || fs::exists(halfPath)) &&
                   std::chrono::steady_clock::now() < resetDeadline) Sleep(1);
            ok = ok && !fs::exists(path) && !fs::exists(gentlerPath) && !fs::exists(halfPath) && fs::exists(otherPath);
        }
        DeleteFileW(otherPath.c_str());
        RemoveDirectoryW(fs::path(path).parent_path().c_str());
        RemoveDirectoryW(fs::path(otherPath).parent_path().c_str());
        RemoveDirectoryW(directory.c_str());
        SetEnvironmentVariableW(L"MOONSR_FG_FULL_CONTEXT",
                                originalFullLength > 0 && originalFullLength < std::size(originalFullMode) ? originalFullMode : nullptr);
        check(ok, "adaptive controller loads, applies, resets and disables a per-game profile");
    }
    for (bool fullContext : {false, true}) {
        FrameGenSettings settings;
        settings.quality = FrameGenQuality::Performance;
        settings.flowScale = 1.0f;
        std::wstring samplerError;
        auto sampler = moonsr::fgadaptive::TripletSampler::Create(gpu->Device(), settings, 0.5f, samplerError, 0.0, fullContext);
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 3> textures;
        auto seqs = MakeSequences(256, 192);
        auto pan = std::find_if(seqs.begin(), seqs.end(), [](const Sequence& s) { return s.name == "pan_5px"; });
        bool ok = sampler && pan != seqs.end();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 320; desc.Height = 224; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 3 && ok; ++i) {
            const Image frame = Render(*pan, 256, 192, i * 0.5, 1);
            std::vector<uint8_t> padded(static_cast<size_t>(desc.Width) * desc.Height * 4);
            for (uint32_t y = 0; y < frame.h; ++y)
                std::copy_n(frame.px.data() + static_cast<size_t>(y) * frame.w * 4, frame.w * 4,
                            padded.data() + (static_cast<size_t>(y + 16) * desc.Width + 32) * 4);
            D3D11_SUBRESOURCE_DATA data{padded.data(), desc.Width * 4, 0};
            ok = SUCCEEDED(gpu->Device().Device()->CreateTexture2D(&desc, &data, &textures[i]));
        }
        const moonsr::RectI rect{32, 16, 288, 208};
        if (ok) {
            const int64_t start = moonsr::QpcNow(), step = moonsr::QpcFrequency() / 60;
            for (int i = 0; i < 3; ++i) sampler->Observe(textures[i].Get(), rect, start + i * step, true);
            sampler->SubmitIfReady(true);
            if (fullContext)
                check(sampler->Ready() && sampler->Busy(), "full-context comparison defers its second GPU submission");
            gpu->Device().Context()->Flush();
            const auto warmupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (sampler->Busy() && std::chrono::steady_clock::now() < warmupDeadline) {
                sampler->SubmitIfReady(true); gpu->Device().Context()->Flush(); sampler->Poll(); Sleep(1);
            }
            ok = !sampler->Busy();
            sampler->Reset();
            for (int i = 0; i < 3; ++i) sampler->Observe(textures[i].Get(), rect, start + (i + 3) * step, true);
            sampler->SubmitIfReady(true);
            gpu->Device().Context()->Flush();
            std::optional<moonsr::fgadaptive::Observation> sample;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!sample && std::chrono::steady_clock::now() < deadline) {
                sampler->SubmitIfReady(true); gpu->Device().Context()->Flush();
                sample = sampler->Poll();
                if (!sample) Sleep(1);
            }
            ok = sample && sample->baselineGpuMs > 0 && sample->candidateGpuMs > 0 &&
                 std::isfinite(sample->baselineError) && std::isfinite(sample->candidateError);
            sampler->Reset();
            sampler->Observe(textures[0].Get(), rect, start + 3 * step, true);
            sampler->Observe(textures[1].Get(), rect, start + 4 * step, false);
            sampler->Observe(textures[2].Get(), rect, start + 5 * step, true);
            sampler->SubmitIfReady(true);
            check(!sampler->Poll(), "adaptive sampler discards a triplet interrupted by a capture discontinuity");
            sampler->Reset();
            for (int i = 0; i < 3; ++i) sampler->Observe(textures[0].Get(), rect, start + (i + 6) * step, true);
            sampler->SubmitIfReady(true);
            gpu->Device().Context()->Flush();
            std::optional<moonsr::fgadaptive::Observation> staticSample;
            const auto staticDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (sampler->Busy() && std::chrono::steady_clock::now() < staticDeadline) {
                sampler->SubmitIfReady(true); gpu->Device().Context()->Flush();
                if (auto value = sampler->Poll()) staticSample = value;
                Sleep(1);
            }
            check(!sampler->Busy() && !staticSample, "adaptive sampler rejects a static triplet");
            sampler->Reset();
            for (int i = 0; i < 3; ++i) sampler->Observe(textures[i].Get(), rect, start + (i + 9) * step, true);
            sampler->SubmitIfReady(true);
            gpu->Device().Context()->Flush();
            std::optional<moonsr::fgadaptive::Observation> nextSample;
            const auto nextDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!nextSample && sampler->Busy() && std::chrono::steady_clock::now() < nextDeadline) {
                sampler->SubmitIfReady(true); gpu->Device().Context()->Flush();
                nextSample = sampler->Poll();
                if (!nextSample) Sleep(1);
            }
            check(sample && nextSample && sample->candidateFirst != nextSample->candidateFirst &&
                  sampler->Diagnostics().motionRejected >= 1 && sampler->Diagnostics().evaluations >= 2,
                  "accepted comparisons alternate timing order even after a rejected static sample");
            sampler->Reset();
            for (int i = 0; i < 3; ++i) sampler->Observe(textures[i].Get(), rect, start + (i + 12) * step, true);
            sampler->SubmitIfReady(true);
            sampler->Reset();
            gpu->Device().Context()->Flush();
            std::optional<moonsr::fgadaptive::Observation> cancelled;
            const auto cancelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (sampler->Busy() && std::chrono::steady_clock::now() < cancelDeadline) {
                if (auto value = sampler->Poll()) cancelled = value;
                Sleep(1);
            }
            check(!sampler->Busy() && !cancelled, "adaptive sampler discards a result after reset");
            sampler->Reset();
            sampler->SetOverloaded(true);
            const auto before = sampler->Diagnostics().triplets;
            for (int i = 0; i < 3; ++i) sampler->Observe(textures[i].Get(), rect, start + (i + 15) * step, true);
            sampler->SubmitIfReady(true);
            gpu->Device().Context()->Flush();
            const auto overloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (sampler->Busy() && std::chrono::steady_clock::now() < overloadDeadline) {
                sampler->SubmitIfReady(true); gpu->Device().Context()->Flush(); sampler->Poll(); Sleep(1);
            }
            for (int i = 0; i < 3; ++i) sampler->Observe(textures[i].Get(), rect, start + (i + 45) * step, true);
            check(!sampler->Busy() && sampler->Diagnostics().triplets == before + (fullContext ? 0 : 1),
                  fullContext ? "overloaded full-context sampler avoids collecting frames"
                              : "overloaded sampler avoids repeated collection until its backoff expires");
            sampler->SetOverloaded(false);
        }
        check(ok, fullContext ? "full-context adaptive sampler evaluates a cropped source without blocking for readback"
                              : "adaptive sampler evaluates a cropped source without blocking for readback");
    }
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter3> memoryAdapter;
        DXGI_QUERY_VIDEO_MEMORY_INFO memoryBefore{}, memoryAfter{};
        const bool haveMemoryInfo = SUCCEEDED(gpu->Device().Adapter()->QueryInterface(IID_PPV_ARGS(&memoryAdapter))) &&
            SUCCEEDED(memoryAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryBefore));
        FrameGenSettings settings;
        settings.quality = FrameGenQuality::Performance;
        settings.flowScale = 1.0f;
        std::wstring samplerError;
        auto sampler = moonsr::fgadaptive::TripletSampler::Create(gpu->Device(), settings, 0.75f, samplerError, 0.0, true);
        auto seqs = MakeSequences(1280, 720);
        auto fence = std::find_if(seqs.begin(), seqs.end(), [](const Sequence& s) { return s.name == "periodic_fence"; });
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 3> textures;
        bool ok = sampler && fence != seqs.end();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 1280; desc.Height = 720; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 3 && ok; ++i) {
            const Image frame = Render(*fence, 1280, 720, i * 0.5, 1);
            D3D11_SUBRESOURCE_DATA data{frame.px.data(), frame.w * 4, 0};
            ok = SUCCEEDED(gpu->Device().Device()->CreateTexture2D(&desc, &data, &textures[i]));
        }
        std::vector<moonsr::fgadaptive::Observation> samples;
        if (ok) {
            const moonsr::RectI rect{0, 0, 1280, 720};
            const int64_t start = moonsr::QpcNow(), step = moonsr::QpcFrequency() / 60;
            for (int attempt = 0; attempt < 3; ++attempt) {
                for (int i = 0; i < 3; ++i) sampler->Observe(textures[i].Get(), rect, start + (attempt * 3 + i) * step, true);
                sampler->SubmitIfReady(true);
                gpu->Device().Context()->Flush();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (sampler->Busy() && std::chrono::steady_clock::now() < deadline) {
                    sampler->SubmitIfReady(true); gpu->Device().Context()->Flush();
                    if (auto value = sampler->Poll()) samples.push_back(*value);
                    Sleep(1);
                }
                if (sampler->Busy()) { ok = false; break; }
                if (attempt < 2) sampler->Reset();
            }
        }
        check(ok && samples.size() == 2 && samples[0].candidateFirst != samples[1].candidateFirst &&
              samples[0].candidateError > samples[0].baselineError * 1.5 + 1.0 &&
              samples[1].candidateError > samples[1].baselineError * 1.5 + 1.0,
              "full-context sampler detects the repeating-pattern regression in both comparison orders");
        if (!samples.empty()) printf("  full-context 720p sample GPU timestamps: baseline %.2f ms, candidate %.2f ms (one sample)\n",
                                     samples[0].baselineGpuMs, samples[0].candidateGpuMs);
        if (haveMemoryInfo && SUCCEEDED(memoryAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryAfter))) {
            const double deltaMiB = (static_cast<double>(memoryAfter.CurrentUsage) - static_cast<double>(memoryBefore.CurrentUsage)) /
                                    (1024.0 * 1024.0);
            printf("  full-context 720p sampler local GPU allocation change: %.1f MiB (driver-reported; approximate)\n", deltaMiB);
        }
    }
    auto& fg = gpu->Fg();
    const uint32_t W = 640, H = 360;
    const Image tex = NoiseImage(W, H, 9);
    {
        const auto sequences = MakeSequences(W, H);
        const auto scene = std::find_if(sequences.begin(), sequences.end(), [](const Sequence& s) { return s.name == "zoom_rotate"; });
        bool ok = scene != sequences.end();
        if (ok) {
            const Image before = Render(*scene, W, H, -1.0), a = Render(*scene, W, H, 0.0), b = Render(*scene, W, H, 1.0);
            FrameGenSettings settings;
            settings.flowScale = 1.0f;
            Image regular, precise;
            ok = gpu->PushSequence({&before, &a, &b}, settings, err) &&
                 gpu->InterpolateRead(1.0f / 3.0f, FrameGenDebugView::None, regular, err);
            settings.preciseMotion = true;
            ok = ok && gpu->PushSequence({&before, &a, &b}, settings, err) &&
                 gpu->InterpolateRead(1.0f / 3.0f, FrameGenDebugView::None, precise, err) && MaxAbsDiff(regular, precise) > 0;
        }
        check(ok, "precise-motion setting changes the generated camera-motion result");
    }
    {
        fg.Reset();
        Image out;
        check(gpu->PushSequence({&tex}, FrameGenSettings{}, err), "single push succeeds");
        check(fg.RealFrame(0) != nullptr && fg.RealFrame(1) == nullptr, "RealFrame(0) set, RealFrame(1) null after one push");
        check(!fg.PairInfo().valid, "PairInfo invalid after one push");
        moonsr::FrameGenDebugView v = FrameGenDebugView::None;
        check(!gpu->InterpolateRead(0.5f, v, out, err), "Interpolate refuses without a pair");
    }
    for (FrameGenQuality q : {FrameGenQuality::Performance, FrameGenQuality::Balanced, FrameGenQuality::Quality}) {
        FrameGenSettings s;
        s.quality = q;
        bool ok = gpu->PushSequence({&tex, &tex, &tex}, s, err);
        double worst = 0;
        for (float t : {0.1f, 0.25f, 0.5f, 0.75f, 0.9f}) {
            Image out;
            ok = ok && gpu->InterpolateRead(t, FrameGenDebugView::None, out, err);
            if (ok) worst = std::max(worst, MaxAbsDiff(out, tex));
        }
        check(ok && worst <= 1, std::string("identical pair reproduces the frame at any t (") + TierName(q) + ", max diff " + Num(worst, 0) + ")");
        check(gpu->CheckUnbound().empty(), std::string("no SRV/UAV/RTV left bound (") + TierName(q) + ") " + gpu->CheckUnbound());
    }
    {
        // Pure integer pan of a random texture: frame B = A shifted by (8,4) px. At t = 0.5 the truth is the (4,2)
        // shift, reconstructable exactly by sampling at integer positions if the flow is exact.
        const Image pa = NoiseImage(W, H, 31, 0, 0), pm = NoiseImage(W, H, 31, 4, 2), pb = NoiseImage(W, H, 31, 8, 4);
        const Image pre = NoiseImage(W, H, 31, -8, -4);
        for (FrameGenQuality q : {FrameGenQuality::Performance, FrameGenQuality::Balanced, FrameGenQuality::Quality}) {
            for (float fsc : {0.25f, 0.5f, 1.0f}) {
                FrameGenSettings s;
                s.quality = q;
                s.flowScale = fsc;
                Image out;
                const bool ok = gpu->PushSequence({&pre, &pa, &pb}, s, err) && gpu->InterpolateRead(0.5f, FrameGenDebugView::None, out, err);
                const Quality qq = ok ? Compare(pm, out, 32, nullptr) : Quality{};
                check(ok && qq.psnrInner > 40.0, std::string("integer pan (8,4) reconstructed at t=0.5, ") + TierName(q) + " flow " +
                                                    Num(fsc, 2) + ": inner PSNR " + Num(qq.psnrInner, 2) + " dB (> 40)");
            }
        }
    }
    {
        const Image ca = NoiseImage(W, H, 41), cb = NoiseImage(W, H, 4242);
        const bool ok = gpu->PushSequence({&ca, &ca, &cb}, FrameGenSettings{}, err) && gpu->WaitIdle();
        const auto info = fg.PairInfo();
        check(ok && info.valid && info.sceneCut, "unrelated pair raises sceneCut (after GPU idle, async readback)");
        Image o1, o2;
        const bool ok2 = gpu->InterpolateRead(0.3f, FrameGenDebugView::None, o1, err) && gpu->InterpolateRead(0.7f, FrameGenDebugView::None, o2, err);
        check(ok2 && MaxAbsDiff(o1, ca) == 0 && MaxAbsDiff(o2, cb) == 0, "scene cut outputs the nearer real frame exactly");
    }
    {
        const Image pa = NoiseImage(W, H, 51, 0, 0), pb = NoiseImage(W, H, 51, 6, 0);
        const bool ok = gpu->PushSequence({&pa, &pb}, FrameGenSettings{}, err) && gpu->WaitIdle();
        const auto info = fg.PairInfo();
        check(ok && info.valid && !info.sceneCut, "related pair (6 px pan) does not raise sceneCut");
        check(ok && std::abs(info.meanMotionPx - 6.0f) < 1.0f, "meanMotionPx ~ 6 for a 6 px pan (got " + Num(info.meanMotionPx, 2) + ")");
    }
    return fails;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    std::string err;
    if (!Parse(argc, argv, o, err)) {
        fprintf(stderr, "%s\n", err.c_str());
        Help();
        return 2;
    }
    if (o.help || (!o.selftest && !o.quality && !o.time && !o.list && o.pair.empty() && o.triplet.empty())) {
        Help();
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (o.list) {
        for (const auto& a : moonsr::GpuDevice::EnumerateAdapters())
            printf("%s  (vendor %04X, %s, FL %X)\n", Narrow(a.name).c_str(), a.vendorId, a.isSoftware ? "software" : "hardware",
                   static_cast<unsigned>(a.maxFeatureLevel));
        return 0;
    }
    const fs::path root = RepoRoot();
    if (o.outDir.empty()) o.outDir = root / "benchmarks" / "output";
    if (o.tag.empty()) {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        char b[32];
        std::strftime(b, sizeof(b), "%Y%m%d_%H%M%S", &tm);
        o.tag = b;
    }
    fs::create_directories(o.outDir / "results");
    moonsr::InitLog((o.outDir / "results" / ("fgbench_" + o.tag + ".log")).wstring());
    const auto adapters = SelectAdapters(o);
    if (adapters.empty() && (o.quality || o.time || !o.pair.empty() || !o.triplet.empty())) {
        fprintf(stderr, "No matching hardware adapter.\n");
        return 2;
    }

    int rc = 0;
    if (o.selftest) {
        std::unique_ptr<Runner> r;
        if (!adapters.empty()) {
            r = Runner::Open(adapters[0], err);
            if (!r) printf("GPU open failed: %s\n", err.c_str());
        }
        const int f = SelfTest(r.get());
        printf("Self-test: %s (%d failure%s)\n", f ? "FAILED" : "OK", f, f == 1 ? "" : "s");
        if (f) rc = 1;
    }

    fs::create_directories(o.outDir / "results");
    std::ofstream json;
    if (o.quality || o.time) {
        json.open(o.outDir / "results" / ("fg_" + o.tag + ".json"));
        json << "{\n  \"tag\": \"" << o.tag << "\",\n  \"note\": \"synthetic sequences only; isolated GPU cost on an idle GPU\",\n";
    }

    if (o.quality) {
        printf("Rendering synthetic sequences %ux%u (supersampled ground truth)...\n", o.w, o.h);
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<SeqData> data;
        std::vector<Sequence> all = MakeSequences(o.w, o.h);
        if (!o.source.empty()) {
            Image src;
            if (!ReadPng(fs::path(o.source).wstring(), src, err)) {
                fprintf(stderr, "--source: %s\n", err.c_str());
                return 2;
            }
            for (auto& s : MakeRealSequences(src, o.w, o.h)) all.push_back(std::move(s));
        }
        for (auto& s : all) {
            if (!o.seqFilter.empty()) {
                bool keep = false;
                for (auto& f : o.seqFilter) keep |= s.name.find(f) != std::string::npos;
                if (!keep) continue;
            }
            SeqData d;
            d.seq = s;
            for (uint32_t i = o.history; i > 0; --i) d.history.push_back(Render(s, o.w, o.h, -static_cast<double>(i)));
            d.a = Render(s, o.w, o.h, 0.0);
            d.b = Render(s, o.w, o.h, 1.0);
            d.ts = kTs;
            if (!s.sceneCut)
                for (double t : d.ts) d.gt.push_back(Render(s, o.w, o.h, t));
            d.hud = RenderHudMask(s, o.w, o.h);
            d.hasHud = static_cast<bool>(s.hudCoverage);
            data.push_back(std::move(d));
        }
        printf("  done in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

        std::ofstream csv(o.outDir / "results" / ("fg_quality_" + o.tag + ".csv"));
        csv << "adapter,sequence,tier,flow_scale,t,method,psnr,psnr_inner48,ssim,psnr_hud,scene_cut_flag,psnr_edge\n";
        json << "  \"quality\": [\n";
        bool firstJ = true;
        // Baselines are adapter independent: computed once.
        std::map<std::string, std::vector<std::pair<Quality, Quality>>> baselines;
        for (auto& d : data) {
            auto& v = baselines[d.seq.name];
            for (size_t k = 0; k < d.gt.size(); ++k)
                v.push_back({Compare(d.gt[k], RepeatNearest(d.a, d.b, d.ts[k]), 48, d.hasHud ? &d.hud : nullptr),
                             Compare(d.gt[k], LinearBlend(d.a, d.b, d.ts[k]), 48, d.hasHud ? &d.hud : nullptr)});
        }
        for (const auto& ad : adapters) {
            auto r = Runner::Open(ad, err);
            const std::string an = Narrow(ad.name);
            if (!r) {
                printf("%s: %s\n", an.c_str(), err.c_str());
                rc = 1;
                continue;
            }
            for (const auto& cfg : o.qualityConfigs) {
                FrameGenSettings s;
                s.quality = cfg.q;
                s.flowScale = cfg.flowScale;
                s.preciseMotion = o.preciseMotion;
                const std::string cfgName = std::string(TierName(cfg.q)) + "_fs" + Num(cfg.flowScale, 2);
                printf("\n== %s | %s | %ux%u ==\n", an.c_str(), cfgName.c_str(), o.w, o.h);
                printf("%-18s %7s | %21s | %21s | %21s | %s\n", "sequence", "t", "MoonSR PSNR/in/SSIM", "blend PSNR/in/SSIM",
                       "repeat PSNR/in/SSIM", "HUD PSNR ours/blend/rep");
                for (auto& d : data) {
                    std::vector<const Image*> frames;
                    for (const auto& frame : d.history) frames.push_back(&frame);
                    frames.push_back(&d.a);
                    frames.push_back(&d.b);
                    if (!r->PushSequence(frames, s, err) || !r->WaitIdle()) {
                        printf("  %s: %s\n", d.seq.name.c_str(), err.c_str());
                        rc = 1;
                        continue;
                    }
                    const auto info = r->Fg().PairInfo();
                    const fs::path imgDir = o.outDir / "fg" / Safe(an) / cfgName / d.seq.name;
                    const bool save = o.saveImages;
                    Image dvFlow, dvConf, dvStatic;
                    if (save) {
                        fs::create_directories(imgDir);
                        Save(imgDir, "A.png", d.a);
                        Save(imgDir, "B.png", d.b);
                        if (r->InterpolateRead(0.5f, FrameGenDebugView::Flow, dvFlow, err)) Save(imgDir, "debug_flow_t500.png", dvFlow);
                        if (r->InterpolateRead(0.5f, FrameGenDebugView::Confidence, dvConf, err)) Save(imgDir, "debug_conf_t500.png", dvConf);
                        if (r->InterpolateRead(0.5f, FrameGenDebugView::StaticMask, dvStatic, err)) Save(imgDir, "debug_static_t500.png", dvStatic);
                    }
                    if (d.seq.sceneCut) {
                        Image o1, o2;
                        const bool ok = r->InterpolateRead(0.25f, FrameGenDebugView::None, o1, err) &&
                                        r->InterpolateRead(0.75f, FrameGenDebugView::None, o2, err);
                        const double d1 = ok ? MaxAbsDiff(o1, d.a) : -1, d2 = ok ? MaxAbsDiff(o2, d.b) : -1;
                        printf("%-18s sceneCut=%s  output@0.25 vs A max diff %s, output@0.75 vs B max diff %s (meanConfidence %.3f)\n",
                               d.seq.name.c_str(), info.sceneCut ? "RAISED" : "not raised", Num(d1, 0).c_str(), Num(d2, 0).c_str(),
                               info.meanConfidence);
                        csv << an << ',' << d.seq.name << ',' << TierName(cfg.q) << ',' << cfg.flowScale << ",0.25,moonsr_vs_A_maxdiff," << d1
                            << ",,,," << info.sceneCut << "\n";
                        csv << an << ',' << d.seq.name << ',' << TierName(cfg.q) << ',' << cfg.flowScale << ",0.75,moonsr_vs_B_maxdiff," << d2
                            << ",,,," << info.sceneCut << "\n";
                        json << (firstJ ? "" : ",\n") << "    {\"adapter\":\"" << an << "\",\"sequence\":\"" << d.seq.name << "\",\"config\":\""
                             << cfgName << "\",\"sceneCut\":" << (info.sceneCut ? "true" : "false") << ",\"maxDiffA\":" << d1
                             << ",\"maxDiffB\":" << d2 << "}";
                        firstJ = false;
                        if (save && ok) {
                            Save(imgDir, "moonsr_t250.png", o1);
                            Save(imgDir, "moonsr_t750.png", o2);
                        }
                        continue;
                    }
                    if (info.sceneCut) printf("  ! %s: sceneCut raised on a related pair\n", d.seq.name.c_str());
                    printf("%-18s pair: meanConfidence %.3f, meanMotionPx %.2f\n", d.seq.name.c_str(), info.meanConfidence, info.meanMotionPx);
                    const auto& bl = baselines[d.seq.name];
                    for (size_t k = 0; k < d.ts.size(); ++k) {
                        const double t = d.ts[k];
                        Image out;
                        if (!r->InterpolateRead(static_cast<float>(t), FrameGenDebugView::None, out, err)) {
                            printf("  %s: %s\n", d.seq.name.c_str(), err.c_str());
                            rc = 1;
                            break;
                        }
                        const Quality q = Compare(d.gt[k], out, 48, d.hasHud ? &d.hud : nullptr);
                        const Quality& qr = bl[k].first;
                        const Quality& qb = bl[k].second;
                        printf("%-18s %7.3f | %6s %6s %6s  | %6s %6s %6s  | %6s %6s %6s  |", d.seq.name.c_str(), t, Num(q.psnr, 2).c_str(),
                               Num(q.psnrInner, 2).c_str(), Num(q.ssim, 4).c_str(), Num(qb.psnr, 2).c_str(), Num(qb.psnrInner, 2).c_str(),
                               Num(qb.ssim, 4).c_str(), Num(qr.psnr, 2).c_str(), Num(qr.psnrInner, 2).c_str(), Num(qr.ssim, 4).c_str());
                        if (d.hasHud) printf(" %s/%s/%s", Num(q.psnrMask, 2).c_str(), Num(qb.psnrMask, 2).c_str(), Num(qr.psnrMask, 2).c_str());
                        printf("  edge %s/%s/%s\n", Num(q.psnrEdge, 2).c_str(), Num(qb.psnrEdge, 2).c_str(), Num(qr.psnrEdge, 2).c_str());
                        const auto row = [&](const char* m, const Quality& qq) {
                            csv << an << ',' << d.seq.name << ',' << TierName(cfg.q) << ',' << cfg.flowScale << ',' << Num(t, 4) << ',' << m << ','
                                << Num(qq.psnr) << ',' << Num(qq.psnrInner) << ',' << Num(qq.ssim, 5) << ',' << (d.hasHud ? Num(qq.psnrMask) : "")
                                << ',' << info.sceneCut << ',' << Num(qq.psnrEdge) << "\n";
                            json << (firstJ ? "" : ",\n") << "    {\"adapter\":\"" << an << "\",\"sequence\":\"" << d.seq.name
                                 << "\",\"config\":\"" << cfgName << "\",\"t\":" << Num(t, 4) << ",\"method\":\"" << m << "\",\"psnr\":"
                                 << (std::isinf(qq.psnr) ? "null" : Num(qq.psnr)) << ",\"psnrInner48\":"
                                 << (std::isinf(qq.psnrInner) ? "null" : Num(qq.psnrInner)) << ",\"ssim\":" << Num(qq.ssim, 5)
                                 << ",\"psnrHud\":" << (d.hasHud ? Num(qq.psnrMask) : "null") << "}";
                            firstJ = false;
                        };
                        row("moonsr", q);
                        row("linear_blend", qb);
                        row("repeat_nearest", qr);
                        if (save) {
                            const std::string tn = TName(t);
                            Save(imgDir, ("gt_" + tn + ".png").c_str(), d.gt[k]);
                            Save(imgDir, ("moonsr_" + tn + ".png").c_str(), out);
                            Save(imgDir, ("blend_" + tn + ".png").c_str(), LinearBlend(d.a, d.b, t));
                            Save(imgDir, ("diff4x_moonsr_" + tn + ".png").c_str(), AbsDiff4(out, d.gt[k]));
                            if (k == 2) {
                                const Image blend = LinearBlend(d.a, d.b, t);
                                for (const auto& c : CropsFor(d.seq.name, d.a.w, d.a.h))
                                    Save(imgDir, ("crop_" + std::string(c.name) + "_gt_moonsr_blend_" + tn + ".png").c_str(),
                                         Mosaic({&d.gt[k], &out, &blend}, c.x, c.y, c.w, c.h, 3));
                                if (!dvFlow.Empty() && !dvConf.Empty() && !dvStatic.Empty())
                                    for (const auto& c : CropsFor(d.seq.name, d.a.w, d.a.h))
                                        Save(imgDir, ("crop_" + std::string(c.name) + "_flow_conf_static_" + tn + ".png").c_str(),
                                             Mosaic({&dvFlow, &dvConf, &dvStatic}, c.x, c.y, c.w, c.h, 3));
                            }
                        }
                    }
                }
            }
        }
        json << "\n  ]" << (o.time ? "," : "") << "\n";
    }

    if (!o.triplet.empty()) {
        Image a, truth, c;
        if (!ReadPng(fs::path(o.triplet[0]).wstring(), a, err) ||
            !ReadPng(fs::path(o.triplet[1]).wstring(), truth, err) ||
            !ReadPng(fs::path(o.triplet[2]).wstring(), c, err)) {
            fprintf(stderr, "--triplet: %s\n", err.c_str());
            return 2;
        }
        if (a.w != truth.w || a.h != truth.h || a.w != c.w || a.h != c.h) {
            fprintf(stderr, "--triplet: all frames must have the same dimensions\n");
            return 2;
        }
        auto runner = Runner::Open(adapters[0], err);
        if (!runner) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        printf("Held-out frame comparison (%ux%u, assumes equally spaced frames; no scene-cut filtering):\n", a.w, a.h);
        std::ofstream csv(o.outDir / "results" / ("fg_triplet_" + o.tag + ".csv"));
        csv << "adapter,width,height,tier,flow_scale,psnr,ssim,scene_cut\n";
        for (const auto& cfg : o.qualityConfigs) {
            FrameGenSettings settings;
            settings.quality = cfg.q;
            settings.flowScale = cfg.flowScale;
            Image generated;
            if (!runner->PushSequence({&a, &c}, settings, err) || !runner->WaitIdle() ||
                !runner->InterpolateRead(0.5f, FrameGenDebugView::None, generated, err)) {
                fprintf(stderr, "--triplet: %s\n", err.c_str());
                rc = 1;
                continue;
            }
            const auto pair = runner->Fg().PairInfo();
            const Quality score = Compare(truth, generated, 0, nullptr);
            printf("  %-12s flow %.2f: PSNR %s dB, SSIM %s, scene cut %s\n", TierName(cfg.q), cfg.flowScale,
                   Num(score.psnr).c_str(), Num(score.ssim, 5).c_str(), pair.sceneCut ? "yes" : "no");
            csv << Narrow(adapters[0].name) << ',' << a.w << ',' << a.h << ',' << TierName(cfg.q) << ',' << cfg.flowScale
                << ',' << Num(score.psnr, 4) << ',' << Num(score.ssim, 6) << ',' << (pair.sceneCut ? 1 : 0) << '\n';
        }
    }

    if (!o.pair.empty()) {
        std::vector<Image> frames(o.pair.size());
        for (size_t i = 0; i < o.pair.size(); ++i)
            if (!ReadPng(fs::path(o.pair[i]).wstring(), frames[i], err)) {
                fprintf(stderr, "--pair %s: %s\n", o.pair[i].c_str(), err.c_str());
                return 2;
            }
        std::vector<const Image*> seq;
        for (auto& f : frames) seq.push_back(&f);
        const Image& A = frames[frames.size() - 2];
        const Image& B = frames.back();
        auto r = Runner::Open(adapters[0], err);
        if (!r) {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        const std::string an = Narrow(adapters[0].name);
        for (const auto& cfg : o.qualityConfigs) {
            FrameGenSettings s;
            s.quality = cfg.q;
            s.flowScale = cfg.flowScale;
            const std::string cfgName = std::string(TierName(cfg.q)) + "_fs" + Num(cfg.flowScale, 2);
            const fs::path dir = o.outDir / "fg_pair" / o.tag / cfgName;
            fs::create_directories(dir);
            if (!r->PushSequence(seq, s, err) || !r->WaitIdle()) {
                printf("%s: %s\n", cfgName.c_str(), err.c_str());
                rc = 1;
                continue;
            }
            const auto info = r->Fg().PairInfo();
            printf("pair %s | %s: sceneCut %d meanConfidence %.3f meanMotionPx %.2f\n", an.c_str(), cfgName.c_str(), info.sceneCut ? 1 : 0,
                   info.meanConfidence, info.meanMotionPx);
            Image g25, g50, g75, dvFlow, dvConf, dvStatic;
            const bool ok = r->InterpolateRead(0.25f, FrameGenDebugView::None, g25, err) &&
                            r->InterpolateRead(0.5f, FrameGenDebugView::None, g50, err) &&
                            r->InterpolateRead(0.75f, FrameGenDebugView::None, g75, err) &&
                            r->InterpolateRead(0.5f, FrameGenDebugView::Flow, dvFlow, err) &&
                            r->InterpolateRead(0.5f, FrameGenDebugView::Confidence, dvConf, err) &&
                            r->InterpolateRead(0.5f, FrameGenDebugView::StaticMask, dvStatic, err);
            if (!ok) {
                printf("  %s\n", err.c_str());
                rc = 1;
                continue;
            }
            Save(dir, "A.png", A);
            Save(dir, "B.png", B);
            Save(dir, "gen_t250.png", g25);
            Save(dir, "gen_t500.png", g50);
            Save(dir, "gen_t750.png", g75);
            Save(dir, "debug_flow_t500.png", dvFlow);
            Save(dir, "debug_conf_t500.png", dvConf);
            Save(dir, "debug_static_t500.png", dvStatic);
            // Regions of the reference capture (ATLYSS 1280x720), scaled to the pair's size.
            const double kx = A.w / 1280.0, ky = A.h / 720.0;
            const Crop crops[] = {{"char", 560, 300, 160, 220}, {"text", 1030, 20, 240, 110}, {"grass", 840, 400, 200, 140},
                                  {"stairs", 560, 220, 220, 110}, {"pillar", 400, 120, 160, 160}, {"hotbar", 400, 630, 260, 90}};
            for (const auto& c : crops) {
                const int x = static_cast<int>(c.x * kx), y = static_cast<int>(c.y * ky);
                const int w = static_cast<int>(c.w * kx), h = static_cast<int>(c.h * ky);
                Save(dir, ("crop_" + std::string(c.name) + "_A_gen_B.png").c_str(), Mosaic({&A, &g50, &B}, x, y, w, h, 3));
                Save(dir, ("crop_" + std::string(c.name) + "_flow_conf_static.png").c_str(),
                     Mosaic({&dvFlow, &dvConf, &dvStatic}, x, y, w, h, 3));
            }
            printf("  images: %s\n", dir.string().c_str());
        }
    }

    if (o.time) {
        std::ofstream csv(o.outDir / "results" / ("fg_timing_" + o.tag + ".csv"));
        csv << "adapter,width,height,tier,flow_scale,stage,samples,mean_ms,median_ms,min_ms,p95_ms,discarded_batches\n";
        json << "  \"timing\": [\n";
        bool firstJ = true;
        std::map<std::pair<uint32_t, uint32_t>, std::pair<Image, Image>> content;
        for (auto sz : o.timeSizes) {
            auto seqs = MakeSequences(sz.first, sz.second);
            content[sz] = {Render(seqs[2], sz.first, sz.second, 0.0, 1), Render(seqs[2], sz.first, sz.second, 1.0, 1)};
            if (!o.source.empty()) {
                Image src;
                if (ReadPng(fs::path(o.source).wstring(), src, err))
                    for (auto& rs : MakeRealSequences(src, sz.first, sz.second))
                        if (rs.name == "real_layers")
                            content[sz] = {Render(rs, sz.first, sz.second, 0.0), Render(rs, sz.first, sz.second, 1.0)};
            }
        }
        for (const auto& ad : adapters) {
            auto r = Runner::Open(ad, err);
            const std::string an = Narrow(ad.name);
            if (!r) {
                printf("%s: %s\n", an.c_str(), err.c_str());
                rc = 1;
                continue;
            }
            printf("\n== timing: %s (isolated GPU cost, idle GPU, %u iterations after %u warm-up) ==\n", an.c_str(), o.iterations, o.warmup);
            printf("%-10s %-12s %5s | %-26s | %-26s\n", "size", "tier", "flow", "PushFrame median/p95 ms", "Interpolate median/p95 ms");
            for (auto sz : o.timeSizes)
                for (auto q : o.timeTiers)
                    for (float fsc : o.timeScales) {
                        FrameGenSettings s;
                        s.quality = q;
                        s.flowScale = fsc;
                        s.preciseMotion = o.preciseMotion;
                        TimingResult tp, ti, pair;
                        if (!r->Time(content[sz].first, content[sz].second, s, o.warmup, o.iterations, tp, ti, pair, err)) {
                            printf("  timing failed: %s\n", err.c_str());
                            rc = 1;
                            continue;
                        }
                        printf("%4ux%-5u %-12s %5.2f | %8s / %8s (n=%u) | %8s / %8s\n", sz.first, sz.second, TierName(q), fsc,
                               Num(tp.medianMs).c_str(), Num(tp.p95Ms).c_str(), tp.samples, Num(ti.medianMs).c_str(), Num(ti.p95Ms).c_str());
                        printf("  2x pair median/p95 %s / %s ms; GPU-only theoretical output %.0f fps (no capture or presentation)\n",
                               Num(pair.medianMs).c_str(), Num(pair.p95Ms).c_str(), pair.medianMs > 0 ? 2000.0 / pair.medianMs : 0.0);
                        for (int k = 0; k < 3; ++k) {
                            const TimingResult& tr = k == 0 ? tp : k == 1 ? ti : pair;
                            const char* stage = k == 0 ? "pushframe" : k == 1 ? "interpolate" : "pair_2x";
                            csv << an << ',' << sz.first << ',' << sz.second << ',' << TierName(q) << ',' << fsc << ',' << stage << ','
                                << tr.samples << ',' << Num(tr.meanMs, 4) << ',' << Num(tr.medianMs, 4) << ',' << Num(tr.minMs, 4) << ','
                                << Num(tr.p95Ms, 4) << ',' << tr.discardedBatches << "\n";
                            json << (firstJ ? "" : ",\n") << "    {\"adapter\":\"" << an << "\",\"width\":" << sz.first << ",\"height\":" << sz.second
                                 << ",\"tier\":\"" << TierName(q) << "\",\"flowScale\":" << fsc << ",\"stage\":\"" << stage
                                 << "\",\"samples\":" << tr.samples << ",\"medianMs\":" << Num(tr.medianMs, 4) << ",\"p95Ms\":" << Num(tr.p95Ms, 4)
                                 << ",\"meanMs\":" << Num(tr.meanMs, 4) << "}";
                            firstJ = false;
                        }
                    }
        }
        json << "\n  ]\n";
    }
    if (json.is_open()) {
        json << "}\n";
        printf("\nResults: %s\n", (o.outDir / "results").string().c_str());
    }
    return rc;
}
