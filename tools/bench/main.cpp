// moonsr_bench: offline upscaling benchmark harness. Runs the real shaders through the real ScalePass on a chosen GPU.
// See documentation/BENCHMARKS.md for methodology and results.
#include "aa_bench.h"
#include "gen.h"
#include "gpu_runner.h"
#include "imageops.h"
#include "metrics.h"
#include "relief_bench.h"
#include "report.h"
#include "temporal.h"

#include <windows.h>

#include <charconv>
#include <chrono>
#include <cstring>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace bench;
using moonsr::AdapterInfo;
using moonsr::ScalerKind;

namespace bench {
int RunSelfTest(GpuRunner* gpu);
}

namespace {

struct Options {
    bool gen = false, selftest = false, quality = false, time = false, listAdapters = false, saveImages = false, verbose = false;
    bool allAdapters = false, help = false, temporal = false, scalesGiven = false, relief = false, reliefNoTime = false;
    std::vector<float> reliefStrengths;
    bool reliefParallax = false;
    bool reliefMotionDepth = false;
    float reliefDetail = -1.0f;
    std::vector<std::string> reliefImages;
    bool aa = false, aaNoTime = false;
    std::vector<std::string> aaImages;
    TemporalOptions tmp;
    uint32_t genW = 1920, genH = 1080;
    std::vector<std::string> adapters;
    std::vector<double> scales{2.0};
    std::vector<float> sharpness{0.0f};
    std::vector<std::string> scalerFilter, imageFilter;
    std::vector<std::pair<moonsr::Size, moonsr::Size>> timeSizes{{{1280, 720}, {1920, 1080}}, {{1920, 1080}, {3840, 2160}}};
    uint32_t iterations = 200, warmup = 50;
    uint32_t idleMs = 0;
    fs::path outDir, gtDir, captureSet;
    std::string tag;
};

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
bool ParseSize(const std::string& s, moonsr::Size& out) {
    unsigned w = 0, h = 0;
    char x = 0;
    std::stringstream ss(s);
    if (!(ss >> w >> x >> h) || (x != 'x' && x != 'X') || w == 0 || h == 0) return false;
    out = {w, h};
    return true;
}
std::string Safe(std::string s) {
    for (auto& c : s)
        if (!isalnum(static_cast<unsigned char>(c))) c = '_';
    return s;
}
std::string FmtNum(double v) {
    char b[32];
    snprintf(b, sizeof(b), "%g", v);
    return b;
}

void PrintHelp() {
    printf(R"(moonsr_bench - offline upscaling benchmark (real ScalePass shaders on a real GPU)

Modes (combine freely):
  --capture-set <dir>    score matched PNGs in input/, native/, moonsr/ and optional dlss/, fsr/, xess/
  --selftest             validate metrics / plumbing; exit code 1 if any check fails
  --gen                  (re)generate the synthetic ground truth into <out>/gt (8-bit sRGB PNG)
  --quality              downsample GT, upscale on the GPU with every available scaler, compute PSNR/SSIM
  --time                 GPU timestamp timing of ScalePass::Render per scaler
  --temporal             temporal sequences (moving content, exact ground truth): luma PSNR, SSIM, error flicker,
                         HUD and scene-cut scores and GPU time per frame for the spatial scalers and MoonSR
  --relief               Smart parallax: synthetic relief scenes with known height (bricks, cobblestones, rock, wood,
                         painted wall, sky, HUD over bricks, silhouette, stylised); height / shading / false-relief /
                         halo / flicker metrics, emboss and clarity baselines, self-checks and GPU time
                         (--relief-image <file> (repeatable; screenshot, static + pan + noisy still)  --relief-strengths 0.5,1
                          --relief-parallax  --relief-detail <game px>  --relief-no-time
                          --save-images writes PNGs to <out>/relief)
  --list-adapters        list adapters and exit
  --relief-motion-depth  test Motion depth with known projective, translational and independent cell motion
  --aa                   Anti-aliasing: synthetic aliased scenes (edges at 0/30/45/60/90 degrees, thin lines, text/HUD
                         over texture, high-frequency texture, gradient); edge-step reduction, texture energy, stroke
                         contrast, HUD protection, gradient safety, self-checks and GPU time
                         (--aa-image <file> (repeatable; real screenshot)  --aa-no-time  --save-images writes to <out>/aa)

Selection:
  --adapter <substr>     adapter name substring (repeatable); default: first hardware adapter
  --all-adapters         all hardware adapters; with --quality also reports cross-adapter pixel differences
  --scalers a,b          only scalers whose name contains one of these substrings (case-insensitive)
  --images a,b           only these ground-truth images (zone_plate, slanted_edges, thin_lines, checker, text,
                         gradient_noise, natural)
  --scales 2,1.5         scale factors for --quality (default 2)
  --sharpness 0,0.5      sharpness values (default 0)
  --size WxH             ground-truth size for --gen (default 1920x1080)
  --time-sizes IxJ:OxP,...  input:output pairs for --time (default 1280x720:1920x1080,1920x1080:3840x2160)
  --iterations N         timed iterations (default 200)   --warmup N  untimed warm-up renders (default 50)
  --idle-ms N            pause N milliseconds between small batches to generate partial GPU load
  --save-images          write upscaled PNGs to <out>/images/<adapter>/<scale>x/
  --verbose              print the per-image quality table
  --out-dir <dir>        default <repo>/benchmarks/output     --gt-dir <dir>  default <out>/gt
Temporal mode (--temporal; uses --adapter, --scales (default 1.5,2), --sharpness, --out-dir, --tag, --save-images):
  --native-frames <dir>  use sorted native-resolution PNGs as reference, downsampled in area mode
  --timing-sweeps N     repeat GPU timing in alternating method order (1-12, default 1)
  --refined-flow         score MoonSR refined vectors with analytic flow in synthetic scenes
  --seqs a,b             pan_slow, pan_medium, pan_fast, rot_zoom, hud_pan, scene_cut, thin_pan, particles, still (default suite); start_stop is opt-in
  --frames N             frames per sequence (default 64)      --seq-warmup N  frames excluded from averages (default 8)
  --seq-size WxH         ground-truth / output size (default 1920x1080)    --seq-ss N  GT supersampling per axis (default 3)
  --input-modes a,b      aliased (point sampled), area (box filtered); default both
  --methods a,b          substrings of: Nearest, Bilinear, Bicubic, Lanczos-3, MoonSR Spatial, MoonSR (temporal; "alsr" also
                         selects it) (default Lanczos-3, MoonSR Spatial, MoonSR). "seed" selects single-frame candidates.
                         "temporal" selects seed trials in temporal reconstruction. "fused" selects inline accumulation; "gated" selects cubic with a zero-gain fast path.
                         "robust" selects thin-feature protection; "tiled" selects the compute registration reference.
                         "point" selects evidence accumulation trials; "auto" selects the GPU sampling detector.
                         "focus" selects stationary reconstruction. "affine", "hud" and "pixel" select motion and overlay trials. "lite" selects the motion-free tier.
  --save-frame N         with --save-images: frame index written as PNG (default 40)
  --tag <name>           result file tag (default: timestamp)
)");
}

bool Parse(int argc, char** argv, Options& o, std::string& err) {
    auto need = [&](int& i) -> const char* {
        if (i + 1 >= argc) { err = std::string("missing value for ") + argv[i]; return nullptr; }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = nullptr;
        if (a == "--gen") o.gen = true;
        else if (a == "--selftest") o.selftest = true;
        else if (a == "--quality") o.quality = true;
        else if (a == "--time") o.time = true;
        else if (a == "--list-adapters") o.listAdapters = true;
        else if (a == "--capture-set") { if (!(v = need(i))) return false; o.captureSet = v; }
        else if (a == "--temporal") { o.temporal = true; }
        else if (a == "--relief") o.relief = true;
        else if (a == "--relief-no-time") o.reliefNoTime = true;
        else if (a == "--relief-parallax") { o.reliefParallax = true; o.relief = true; }
        else if (a == "--relief-motion-depth") o.reliefMotionDepth = true;
        else if (a == "--aa") o.aa = true;
        else if (a == "--aa-no-time") o.aaNoTime = true;
        else if (a == "--aa-image") { if (!(v = need(i))) return false; o.aaImages.push_back(v); o.aa = true; }
        else if (a == "--relief-image") { if (!(v = need(i))) return false; o.reliefImages.push_back(v); o.relief = true; }
        else if (a == "--relief-detail") { if (!(v = need(i))) return false; o.reliefDetail = static_cast<float>(atof(v)); }
        else if (a == "--relief-strengths") { if (!(v = need(i))) return false; for (auto& t : Split(v, ',')) o.reliefStrengths.push_back(static_cast<float>(atof(t.c_str()))); }
        else if (a == "--native-frames") { if (!(v = need(i))) return false; o.tmp.nativeFrames = v; o.temporal = true; }
        else if (a == "--timing-sweeps") {
            if (!(v = need(i))) return false;
            const auto end = v + strlen(v);
            const auto parsed = std::from_chars(v, end, o.tmp.timingSweeps);
            if (parsed.ec != std::errc{} || parsed.ptr != end || o.tmp.timingSweeps < 1 || o.tmp.timingSweeps > 12) {
                err = "--timing-sweeps must be an integer from 1 to 12";
                return false;
            }
        }
        else if (a == "--refined-flow") { o.tmp.scoreRefinedFlow = true; o.temporal = true; }
        else if (a == "--seqs") { if (!(v = need(i))) return false; o.tmp.seqs = Split(v, ','); }
        else if (a == "--input-modes") { if (!(v = need(i))) return false; o.tmp.modes = Split(v, ','); for (auto& s : o.tmp.modes) if (s != "aliased" && s != "area") { err = "bad --input-modes"; return false; } }
        else if (a == "--methods") { if (!(v = need(i))) return false; for (auto& s : Split(v, ',')) o.tmp.methods.push_back(Lower(s)); }
        else if (a == "--frames") { if (!(v = need(i))) return false; o.tmp.frames = atoi(v); }
        else if (a == "--seq-warmup") { if (!(v = need(i))) return false; o.tmp.warmup = std::max(0, atoi(v)); }
        else if (a == "--seq-ss") { if (!(v = need(i))) return false; o.tmp.ss = std::clamp(atoi(v), 1, 8); }
        else if (a == "--save-frame") { if (!(v = need(i))) return false; o.tmp.saveFrame = std::max(0, atoi(v)); }
        else if (a == "--seq-size") {
            if (!(v = need(i))) return false;
            moonsr::Size s;
            if (!ParseSize(v, s)) { err = "bad --seq-size (expected WxH)"; return false; }
            o.tmp.gtW = s.width; o.tmp.gtH = s.height;
        }
        else if (a == "--save-images") o.saveImages = true;
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--all-adapters") o.allAdapters = true;
        else if (a == "--help" || a == "-h" || a == "/?") o.help = true;
        else if (a == "--adapter") { if (!(v = need(i))) return false; o.adapters.push_back(v); }
        else if (a == "--scalers") { if (!(v = need(i))) return false; for (auto& s : Split(v, ',')) o.scalerFilter.push_back(Lower(s)); }
        else if (a == "--images") { if (!(v = need(i))) return false; o.imageFilter = Split(v, ','); }
        else if (a == "--scales") {
            if (!(v = need(i))) return false;
            o.scales.clear();
            o.scalesGiven = true;
            for (auto& s : Split(v, ',')) o.scales.push_back(atof(s.c_str()));
            for (double s : o.scales) if (!(s >= 1.0 && s <= 16.0)) { err = "bad --scales value"; return false; }
        } else if (a == "--sharpness") {
            if (!(v = need(i))) return false;
            o.sharpness.clear();
            for (auto& s : Split(v, ',')) o.sharpness.push_back(static_cast<float>(atof(s.c_str())));
        } else if (a == "--size") {
            if (!(v = need(i))) return false;
            moonsr::Size s;
            if (!ParseSize(v, s)) { err = "bad --size (expected WxH)"; return false; }
            o.genW = s.width; o.genH = s.height;
        } else if (a == "--time-sizes") {
            if (!(v = need(i))) return false;
            o.timeSizes.clear();
            for (auto& pair : Split(v, ',')) {
                const auto parts = Split(pair, ':');
                moonsr::Size in, out;
                if (parts.size() != 2 || !ParseSize(parts[0], in) || !ParseSize(parts[1], out)) { err = "bad --time-sizes (expected IxJ:OxP,...)"; return false; }
                o.timeSizes.push_back({in, out});
            }
        } else if (a == "--iterations") { if (!(v = need(i))) return false; o.iterations = static_cast<uint32_t>(std::max(1, atoi(v))); }
        else if (a == "--idle-ms") { if (!(v = need(i))) return false; o.idleMs = static_cast<uint32_t>(std::clamp(atoi(v), 0, 1000)); }
        else if (a == "--warmup") { if (!(v = need(i))) return false; o.warmup = static_cast<uint32_t>(std::max(0, atoi(v))); }
        else if (a == "--out-dir") { if (!(v = need(i))) return false; o.outDir = v; }
        else if (a == "--gt-dir") { if (!(v = need(i))) return false; o.gtDir = v; }
        else if (a == "--tag") { if (!(v = need(i))) return false; o.tag = Safe(v); }
        else { err = "unknown option " + a; return false; }
    }
    return true;
}

bool ScalerSelected(const Options& o, const std::string& name) {
    if (o.scalerFilter.empty()) return true;
    const std::string n = Lower(name);
    for (const auto& f : o.scalerFilter) if (n.find(f) != std::string::npos) return true;
    return false;
}

std::vector<AdapterInfo> PickAdapters(const Options& o, std::string& err) {
    std::vector<AdapterInfo> hw;
    for (auto& a : moonsr::GpuDevice::EnumerateAdapters())
        if (!a.isSoftware) hw.push_back(a);
    if (hw.empty()) { err = "no hardware adapter found"; return {}; }
    if (o.allAdapters) return hw;
    if (o.adapters.empty()) return {hw.front()};
    std::vector<AdapterInfo> out;
    for (const auto& sub : o.adapters) {
        bool found = false;
        for (auto& a : hw) {
            bool already = false;
            for (auto& s : out) already |= moonsr::LuidEqual(s.luid, a.luid);
            if (already) continue;
            if (Lower(Narrow(a.name)).find(Lower(sub)) != std::string::npos) { out.push_back(a); found = true; break; }
        }
        if (!found) { err = "no (unselected) hardware adapter matches '" + sub + "'"; return {}; }
    }
    return out;
}

struct GtImage {
    std::string name;
    Image img;
    std::string hash;
};

std::string HashHex(const Image& im) {
    char b[32];
    snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(Fnv1a(im.px.data(), im.px.size())));
    return b;
}

bool LoadGroundTruth(const Options& o, bool forceGen, std::vector<GtImage>& out, std::string& err) {
    std::error_code ec;
    fs::create_directories(o.gtDir, ec);
    for (const auto& name : GeneratorNames()) {
        if (!o.imageFilter.empty() && std::find(o.imageFilter.begin(), o.imageFilter.end(), name) == o.imageFilter.end()) continue;
        const fs::path p = o.gtDir / (name + ".png");
        GtImage g;
        g.name = name;
        if (!forceGen && fs::exists(p)) {
            if (!ReadPng(p.wstring(), g.img, err)) return false;
        } else {
            const auto t0 = std::chrono::steady_clock::now();
            if (!Generate(name, o.genW, o.genH, g.img)) { err = "unknown generator " + name; return false; }
            if (!WritePng(p.wstring(), g.img, err)) return false;
            printf("generated %-15s %ux%u in %.2f s -> %s\n", name.c_str(), g.img.w, g.img.h,
                   std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), p.string().c_str());
        }
        g.hash = HashHex(g.img);
        out.push_back(std::move(g));
    }
    if (out.empty()) { err = "no ground-truth images selected"; return false; }
    return true;
}

std::string FileTag(const Options& o, const SystemInfo& si) {
    if (!o.tag.empty()) return o.tag;
    std::string d = si.date;
    for (auto& c : d) if (c == '-' || c == ':' || c == ' ') c = (c == ' ') ? '_' : '\0';
    d.erase(std::remove(d.begin(), d.end(), '\0'), d.end());
    return d;
}

Record Meta(const SystemInfo& si) {
    return {S("tool", "moonsr_bench"), S("os_build", si.osBuild), S("cpu", si.cpu), S("git_commit", si.gitCommit),
            S("source_hash", si.sourceHash), S("date", si.date), S("command_line", si.commandLine)};
}

void Concat(Record& a, const Record& b) { a.insert(a.end(), b.begin(), b.end()); }

void Save(const fs::path& base, const std::string& stem, const Record& meta, const std::vector<Record>& rows) {
    std::string err;
    if (!WriteCsv(base / (stem + ".csv"), rows, err)) printf("warning: %s\n", err.c_str());
    else printf("wrote %s\n", (base / (stem + ".csv")).string().c_str());
    if (!WriteJson(base / (stem + ".json"), meta, rows, err)) printf("warning: %s\n", err.c_str());
    else printf("wrote %s\n", (base / (stem + ".json")).string().c_str());
}

// ---------------------------------------------------------------------------------------------------------------------

int RunQuality(const Options& o, const std::vector<AdapterInfo>& adapters, const SystemInfo& si) {
    std::string err;
    std::vector<GtImage> gts;
    if (!LoadGroundTruth(o, false, gts, err)) { printf("error: %s\n", err.c_str()); return 1; }

    std::vector<std::unique_ptr<GpuRunner>> runners;
    for (auto& a : adapters) {
        auto r = GpuRunner::Open(a, err);
        if (!r) { printf("error: adapter '%s': %s\n", Narrow(a.name).c_str(), err.c_str()); return 1; }
        runners.push_back(std::move(r));
    }
    std::vector<moonsr::ScalerDesc> scalers;
    for (auto& s : moonsr::ScalePass::ListScalers()) {
        if (!s.available) { printf("note: scaler '%s' is not part of this build (shader not compiled in)\n", Narrow(s.name).c_str()); continue; }
        if (ScalerSelected(o, Narrow(s.name))) scalers.push_back(s);
    }
    printf("\nScalers measured:");
    for (auto& s : scalers) printf(" [%s]", Narrow(s.name).c_str());
    printf("\nGround truth: %ux%u, %zu images\n", gts[0].img.w, gts[0].img.h, gts.size());

    std::vector<Record> rows;
    int failures = 0;
    for (double scale : o.scales) {
        for (const GtImage& gt : gts) {
            const uint32_t iw = ScaledExtent(gt.img.w, scale), ih = ScaledExtent(gt.img.h, scale);
            const Image input = AreaDownsample(gt.img, iw, ih);
            const std::string scaleStr = FmtNum(scale);
            if (o.saveImages) {
                const fs::path d = o.outDir / "images" / (scaleStr + "x");
                std::error_code ec;
                fs::create_directories(d, ec);
                WritePng((d / (gt.name + "_input_lr.png")).wstring(), input, err);
            }
            // Cache of the first adapter's outputs for this image, for the cross-adapter comparison.
            std::map<std::pair<int, int>, Image> ref;
            for (size_t ri = 0; ri < runners.size(); ++ri) {
                GpuRunner& gpu = *runners[ri];
                for (size_t si_ = 0; si_ < scalers.size(); ++si_) {
                    for (size_t shi = 0; shi < o.sharpness.size(); ++shi) {
                        const float sh = o.sharpness[shi];
                        Image up;
                        if (!gpu.Upscale(input, gt.img.w, gt.img.h, scalers[si_].kind, sh, up, err)) {
                            printf("error: %s %s: %s\n", Narrow(gpu.Info().name).c_str(), Narrow(scalers[si_].name).c_str(), err.c_str());
                            ++failures;
                            continue;
                        }
                        const Metrics m = Compare(gt.img, up);
                        Record r = {S("kind", "quality")};
                        Concat(r, AdapterFields(gpu.Info()));
                        Concat(r, RunFields(si));
                        r.push_back(N("scale", scale, 3));
                        r.push_back(I("gt_w", gt.img.w)); r.push_back(I("gt_h", gt.img.h));
                        r.push_back(I("in_w", iw)); r.push_back(I("in_h", ih));
                        r.push_back(S("image", gt.name)); r.push_back(S("gt_fnv1a", gt.hash));
                        r.push_back(S("scaler", Narrow(scalers[si_].name)));
                        r.push_back(N("sharpness", sh, 3));
                        r.push_back(N("psnr_rgb_db", m.psnrRgb, 3)); r.push_back(N("psnr_luma_db", m.psnrLuma, 3));
                        r.push_back(N("ssim", m.ssim, 5)); r.push_back(N("max_abs_err", m.maxAbs, 0));
                        rows.push_back(r);

                        if (o.saveImages) {
                            char sb[32];
                            snprintf(sb, sizeof(sb), "%g", sh);
                            const fs::path d = o.outDir / "images" / (scaleStr + "x") / Safe(Narrow(gpu.Info().name));
                            std::error_code ec;
                            fs::create_directories(d, ec);
                            WritePng((d / (gt.name + "_" + Safe(Narrow(scalers[si_].name)) + "_s" + sb + ".png")).wstring(), up, err);
                        }

                        // Cross-adapter comparison against the first adapter.
                        const auto key = std::make_pair(static_cast<int>(si_), static_cast<int>(shi));
                        if (runners.size() > 1) {
                            if (ri == 0) {
                                ref[key] = up;
                            } else {
                                const DiffStats d = Diff(ref[key], up);
                                Record x = {S("kind", "cross_adapter"), S("adapter_ref", Narrow(runners[0]->Info().name)),
                                            S("adapter", Narrow(gpu.Info().name))};
                                Concat(x, RunFields(si));
                                x.push_back(N("scale", scale, 3));
                                x.push_back(S("image", gt.name));
                                x.push_back(S("scaler", Narrow(scalers[si_].name)));
                                x.push_back(N("sharpness", sh, 3));
                                x.push_back(N("max_abs_diff", d.maxAbs, 0));
                                x.push_back(N("mean_abs_diff", d.meanAbs, 6));
                                x.push_back(I("differing_pixels", static_cast<long long>(d.differing)));
                                x.push_back(N("differing_pixels_pct", 100.0 * d.differing / d.pixels, 4));
                                rows.push_back(x);
                            }
                        }

                        // Alignment check: GPU bilinear vs a CPU bilinear with correct centre alignment.
                        if (scalers[si_].kind == ScalerKind::Bilinear && sh == 0.0f) {
                            const Image cpu = BilinearUpscale(input, gt.img.w, gt.img.h, 0.0);
                            const DiffStats d0 = Diff(cpu, up);
                            const Metrics mc = Compare(cpu, up);
                            const Metrics mgt = Compare(gt.img, cpu);
                            Record a = {S("kind", "alignment")};
                            Concat(a, AdapterFields(gpu.Info()));
                            Concat(a, RunFields(si));
                            a.push_back(N("scale", scale, 3));
                            a.push_back(S("image", gt.name));
                            a.push_back(N("gpu_vs_cpu_psnr_rgb_db", mc.psnrRgb, 3));
                            a.push_back(N("gpu_vs_cpu_max_abs", d0.maxAbs, 0));
                            a.push_back(N("gpu_vs_cpu_mean_abs", d0.meanAbs, 5));
                            // Same comparison against references shifted by +-0.5 source texel (the failure we want to detect).
                            for (double shift : {-0.5, 0.5}) {
                                const Metrics ms = Compare(BilinearUpscale(input, gt.img.w, gt.img.h, shift), up);
                                a.push_back(N(shift < 0 ? "gpu_vs_cpu_shiftm0p5_psnr_rgb_db" : "gpu_vs_cpu_shiftp0p5_psnr_rgb_db", ms.psnrRgb, 3));
                            }
                            a.push_back(N("cpu_bilinear_vs_gt_psnr_rgb_db", mgt.psnrRgb, 3));
                            a.push_back(N("gpu_bilinear_vs_gt_psnr_rgb_db", m.psnrRgb, 3));
                            rows.push_back(a);
                        }
                    }
                }
            }
        }
    }

    // ---- console summaries -------------------------------------------------------------------------------------
    struct Acc { double psnr = 0, luma = 0, ssim = 0; int n = 0; bool inf = false; };
    std::map<std::string, Acc> acc;
    std::vector<std::string> order;
    std::vector<Record> verboseRows, sumRows;
    for (const auto& r : rows) {
        auto get = [&](const char* k) { for (auto& f : r) if (f.key == k) return f.val; return std::string(); };
        if (get("kind") != "quality") continue;
        const std::string key = get("adapter") + "|" + get("scale") + "|" + get("scaler") + "|" + get("sharpness");
        if (!acc.count(key)) order.push_back(key);
        Acc& a = acc[key];
        const std::string p = get("psnr_rgb_db"), l = get("psnr_luma_db");
        if (p == "inf" || l == "inf") a.inf = true;
        else { a.psnr += atof(p.c_str()); a.luma += atof(l.c_str()); }
        a.ssim += atof(get("ssim").c_str());
        ++a.n;
        verboseRows.push_back({S("adapter", get("adapter")), S("scale", get("scale")), S("image", get("image")), S("scaler", get("scaler")),
                               S("sharp", get("sharpness")), S("PSNR_rgb", p), S("PSNR_luma", l), S("SSIM", get("ssim")), S("maxerr", get("max_abs_err"))});
    }
    for (const auto& key : order) {
        const Acc& a = acc[key];
        const auto parts = [&] { std::vector<std::string> v; std::stringstream ss(key); std::string t; while (std::getline(ss, t, '|')) v.push_back(t); return v; }();
        sumRows.push_back({S("adapter", parts[0]), S("scale", parts[1]), S("scaler", parts[2]), S("sharp", parts[3]),
                           S("PSNR_rgb", a.inf ? "inf" : Num(a.psnr / a.n, 2)), S("PSNR_luma", a.inf ? "inf" : Num(a.luma / a.n, 2)),
                           S("SSIM", Num(a.ssim / a.n, 4)), S("images", std::to_string(a.n))});
    }
    if (o.verbose) {
        printf("\n== Per-image quality ==\n");
        PrintTable(verboseRows, {"adapter", "scale", "image", "scaler", "sharp", "PSNR_rgb", "PSNR_luma", "SSIM", "maxerr"});
    }
    printf("\n== Quality summary (mean over images; PSNR in dB is the arithmetic mean of per-image dB) ==\n");
    PrintTable(sumRows, {"adapter", "scale", "scaler", "sharp", "PSNR_rgb", "PSNR_luma", "SSIM", "images"});

    std::vector<Record> alignRows, crossRows;
    for (const auto& r : rows) {
        auto get = [&](const char* k) { for (auto& f : r) if (f.key == k) return f.val; return std::string(); };
        if (get("kind") == "alignment")
            alignRows.push_back({S("adapter", get("adapter")), S("scale", get("scale")), S("image", get("image")),
                                 S("GPUvsCPU_PSNR", get("gpu_vs_cpu_psnr_rgb_db")), S("maxabs", get("gpu_vs_cpu_max_abs")),
                                 S("meanabs", get("gpu_vs_cpu_mean_abs")), S("vs_shift-0.5", get("gpu_vs_cpu_shiftm0p5_psnr_rgb_db")),
                                 S("vs_shift+0.5", get("gpu_vs_cpu_shiftp0p5_psnr_rgb_db")), S("cpuBil_vs_GT", get("cpu_bilinear_vs_gt_psnr_rgb_db")),
                                 S("gpuBil_vs_GT", get("gpu_bilinear_vs_gt_psnr_rgb_db"))});
        else if (get("kind") == "cross_adapter")
            crossRows.push_back({S("scale", get("scale")), S("image", get("image")), S("scaler", get("scaler")), S("sharp", get("sharpness")),
                                 S("adapter", get("adapter")), S("max_abs_diff", get("max_abs_diff")), S("mean_abs_diff", get("mean_abs_diff")),
                                 S("diff_px_%", get("differing_pixels_pct"))});
    }
    if (!alignRows.empty()) {
        printf("\n== Alignment check: GPU bilinear vs CPU bilinear (centre aligned) and vs references shifted by +-0.5 source texel ==\n");
        PrintTable(alignRows, {"adapter", "scale", "image", "GPUvsCPU_PSNR", "maxabs", "meanabs", "vs_shift-0.5", "vs_shift+0.5", "cpuBil_vs_GT", "gpuBil_vs_GT"});
    }
    if (!crossRows.empty()) {
        printf("\n== Cross-adapter pixel difference (reference adapter: %s) ==\n", Narrow(adapters[0].name).c_str());
        // Condense to the worst case over images per (adapter, scale, scaler, sharp).
        std::map<std::string, Record> worst;
        std::vector<std::string> ord;
        auto num = [](const Record& rr, const char* k) { for (auto& f : rr) if (f.key == k) return atof(f.val.c_str()); return 0.0; };
        for (const auto& r : crossRows) {
            std::string key;
            for (const char* k : {"adapter", "scale", "scaler", "sharp"}) for (auto& f : r) if (f.key == k) key += f.val + "|";
            if (!worst.count(key)) { worst[key] = r; ord.push_back(key); continue; }
            const Record& w = worst[key];
            if (num(r, "max_abs_diff") > num(w, "max_abs_diff") ||
                (num(r, "max_abs_diff") == num(w, "max_abs_diff") && num(r, "diff_px_%") > num(w, "diff_px_%"))) worst[key] = r;
        }
        std::vector<Record> cond;
        for (auto& k : ord) cond.push_back(worst[k]);
        printf("(worst image per row, by max_abs_diff then differing-pixel share; per-image values are in the CSV/JSON)\n");
        PrintTable(cond, {"adapter", "scale", "scaler", "sharp", "image", "max_abs_diff", "mean_abs_diff", "diff_px_%"});
    }

    Save(o.outDir / "results", "quality_" + FileTag(o, si), Meta(si), rows);
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------

int RunTiming(const Options& o, const std::vector<AdapterInfo>& adapters, const SystemInfo& si) {
    std::string err;
    std::vector<moonsr::ScalerDesc> scalers;
    for (auto& s : moonsr::ScalePass::ListScalers()) {
        if (!s.available) { printf("note: scaler '%s' is not part of this build (shader not compiled in)\n", Narrow(s.name).c_str()); continue; }
        if (ScalerSelected(o, Narrow(s.name))) scalers.push_back(s);
    }
    printf("\nTIMING: isolated ScalePass::Render pass time (clear + draw, plus sharpen stage when sharpness > 0) measured with\n"
           "D3D11 timestamp queries on an otherwise idle GPU. This is NOT in-game cost: it excludes capture, present, and\n"
           "contention with a running game. Each iteration is bracketed by its own pair of timestamps inside a disjoint query;\n"
           "batches reported disjoint are discarded. Warm-up: %u renders, then %u timed iterations per row.\n",
           o.warmup, o.iterations);

    std::vector<Record> rows;
    int failures = 0;
    for (const AdapterInfo& a : adapters) {
        auto gpu = GpuRunner::Open(a, err);
        if (!gpu) { printf("error: adapter '%s': %s\n", Narrow(a.name).c_str(), err.c_str()); ++failures; continue; }
        printf("\n-- Adapter: %s  (driver UMD %s, VEN_%04X DEV_%04X)\n", Narrow(a.name).c_str(), DriverVersionString(a.driverVersion).c_str(), a.vendorId, a.deviceId);
        std::vector<Record> shown;
        for (const auto& [in, out] : o.timeSizes) {
            Image content;
            Generate("natural", in.width, in.height, content);
            for (const auto& sc : scalers) {
                for (float sh : o.sharpness) {
                    TimingParams p;
                    p.inW = in.width; p.inH = in.height; p.outW = out.width; p.outH = out.height;
                    p.scaler = sc.kind; p.sharpness = sh; p.iterations = o.iterations; p.warmup = o.warmup;
                    p.idleMs = o.idleMs;
                    TimingResult t;
                    if (!gpu->Time(p, content, t, err)) {
                        printf("error: %s %ux%u->%ux%u %s: %s\n", Narrow(a.name).c_str(), in.width, in.height, out.width, out.height, Narrow(sc.name).c_str(), err.c_str());
                        ++failures;
                        continue;
                    }
                    Record r = {S("kind", "timing")};
                    Concat(r, AdapterFields(a));
                    Concat(r, RunFields(si));
                    r.push_back(I("in_w", in.width)); r.push_back(I("in_h", in.height));
                    r.push_back(I("out_w", out.width)); r.push_back(I("out_h", out.height));
                    r.push_back(S("scaler", Narrow(sc.name)));
                    r.push_back(N("sharpness", sh, 3));
                    r.push_back(I("iterations", t.samples));
                    r.push_back(I("warmup", o.warmup));
                    r.push_back(I("discarded_disjoint_batches", t.discardedBatches));
                    r.push_back(I("timestamp_freq_hz", static_cast<long long>(t.timestampFreq)));
                    r.push_back(N("mean_ms", t.meanMs, 4)); r.push_back(N("median_ms", t.medianMs, 4));
                    r.push_back(N("min_ms", t.minMs, 4)); r.push_back(N("p95_ms", t.p95Ms, 4)); r.push_back(N("max_ms", t.maxMs, 4));
                    r.push_back(N("wall_per_iter_ms", t.wallPerIterMs, 4));
                    rows.push_back(r);
                    char sz[64];
                    snprintf(sz, sizeof(sz), "%ux%u->%ux%u", in.width, in.height, out.width, out.height);
                    shown.push_back({S("size", sz), S("scaler", Narrow(sc.name)), S("sharp", Num(sh, 2)), N("mean_ms", t.meanMs, 4),
                                     N("median_ms", t.medianMs, 4), N("min_ms", t.minMs, 4), N("p95_ms", t.p95Ms, 4), N("max_ms", t.maxMs, 4),
                                     N("wall_ms", t.wallPerIterMs, 4), I("n", t.samples), I("disjoint", t.discardedBatches)});
                }
            }
        }
        PrintTable(shown, {"size", "scaler", "sharp", "mean_ms", "median_ms", "min_ms", "p95_ms", "max_ms", "wall_ms", "n", "disjoint"});
    }
    Save(o.outDir / "results", "timing_" + FileTag(o, si), Meta(si), rows);
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Options o;
    std::string err;
    if (!Parse(argc, argv, o, err)) {
        printf("error: %s\n\n", err.c_str());
        PrintHelp();
        return 2;
    }
    if (o.help || (!o.gen && !o.selftest && !o.quality && !o.time && !o.temporal && !o.relief && !o.reliefMotionDepth && !o.aa && !o.listAdapters && o.captureSet.empty())) {
        PrintHelp();
        return o.help ? 0 : 2;
    }
    const HRESULT chr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(chr)) { printf("error: CoInitializeEx failed\n"); return 1; }

    const fs::path root = FindRepoRoot();
    if (o.outDir.empty()) o.outDir = root / "benchmarks" / "output";
    if (o.gtDir.empty()) o.gtDir = o.outDir / "gt";

    std::string cmdline = "moonsr_bench";
    for (int i = 1; i < argc; ++i) cmdline += std::string(" ") + argv[i];
    const SystemInfo si = CollectSystemInfo(root, cmdline);
    printf("moonsr_bench | OS %s | git %s | sources %s | %s\n", si.osBuild.c_str(), si.gitCommit.c_str(), si.sourceHash.c_str(), si.date.c_str());

    int rc = 0;
    if (!o.captureSet.empty()) rc |= ScoreCaptureSet(o.captureSet, o.outDir / "results", FileTag(o, si));
    if (o.listAdapters) {
        for (auto& a : moonsr::GpuDevice::EnumerateAdapters())
            printf("%s%s | driver UMD %s | VEN_%04X DEV_%04X | VRAM %llu MiB\n", Narrow(a.name).c_str(), a.isSoftware ? " [software]" : "",
                   DriverVersionString(a.driverVersion).c_str(), a.vendorId, a.deviceId, static_cast<unsigned long long>(a.dedicatedVideoMemory >> 20));
        if (!o.gen && !o.selftest && !o.quality && !o.time && !o.temporal) return rc;
    }
    if (o.gen) {
        std::vector<GtImage> gts;
        if (!LoadGroundTruth(o, true, gts, err)) { printf("error: %s\n", err.c_str()); return 1; }
        printf("ground truth hashes (FNV-1a of BGRA bytes):\n");
        for (auto& g : gts) printf("  %-15s %ux%u %s\n", g.name.c_str(), g.img.w, g.img.h, g.hash.c_str());
    }

    std::vector<AdapterInfo> adapters;
    if (o.selftest || o.quality || o.time || o.temporal || o.relief || o.reliefMotionDepth || o.aa) {
        adapters = PickAdapters(o, err);
        if (adapters.empty()) {
            if (o.selftest && !o.quality && !o.time) printf("warning: %s; GPU self-checks skipped\n", err.c_str());
            else { printf("error: %s\n", err.c_str()); return 1; }
        }
    }
    if (o.selftest) {
        std::unique_ptr<GpuRunner> gpu;
        if (!adapters.empty()) {
            gpu = GpuRunner::Open(adapters[0], err);
            if (!gpu) printf("warning: could not open adapter for GPU self-checks: %s\n", err.c_str());
        }
        if (RunSelfTest(gpu.get()) != 0) rc = 1;
    }
    if (o.quality && !adapters.empty()) rc |= RunQuality(o, adapters, si);
    if (o.time && !adapters.empty()) rc |= RunTiming(o, adapters, si);
    if (o.temporal && !adapters.empty()) {
        std::vector<std::unique_ptr<GpuRunner>> runners;
        for (auto& a : adapters) {
            auto r = GpuRunner::Open(a, err);
            if (!r) { printf("error: adapter '%s': %s\n", Narrow(a.name).c_str(), err.c_str()); return 1; }
            runners.push_back(std::move(r));
        }
        TemporalOptions t = o.tmp;
        t.scales = o.scalesGiven ? o.scales : std::vector<double>{1.5, 2.0};
        t.sharpness = o.sharpness;
        t.saveImages = o.saveImages;
        t.outDir = o.outDir;
        rc |= RunTemporalBench(t, runners, si, "t_" + FileTag(o, si));
    }
    if (o.relief && !adapters.empty()) {
        ReliefBenchOptions r;
        if (!o.reliefStrengths.empty()) r.strengths = o.reliefStrengths;
        r.detailScalePx = o.reliefDetail;
        r.timing = !o.reliefNoTime;
        r.parallax = o.reliefParallax;
        r.saveImages = o.saveImages;
        r.outDir = o.outDir / "relief";
        for (const auto& im : o.reliefImages) r.images.emplace_back(im);
        for (auto& a : adapters) rc |= r.images.empty() ? RunReliefBench(r, a) : RunReliefImage(r, a);
    }
    if (o.reliefMotionDepth)
        for (const auto& a : adapters) rc |= RunReliefMotionBench(a);
    if (o.aa && !adapters.empty()) {
        AaBenchOptions aa;
        aa.timing = !o.aaNoTime;
        aa.saveImages = o.saveImages;
        aa.outDir = o.outDir / "aa";
        for (const auto& im : o.aaImages) aa.images.emplace_back(im);
        for (auto& a : adapters) rc |= aa.images.empty() ? RunAaBench(aa, a) : RunAaImage(aa, a);
    }
    CoUninitialize();
    return rc;
}
