// Temporal-sequence benchmark: procedural moving scenes with exact ground truth, rendered at output resolution and
// reduced to game resolution (point-sampled = aliased, or area-averaged = anti-aliased), upscaled frame by frame by
// the spatial scalers (ScalePass) or by MoonSR temporal (AlsrUpscaler fed by a FrameGenerator on the same device).
#pragma once
#include "common.h"
#include "gpu_runner.h"
#include "report.h"
#include "framegen/frame_generator.h"
#include "upscale/alsr_upscaler.h"

#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace bench {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct HudRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // [x0,x1) x [y0,y1) in ground-truth pixels
};

// A procedural scene f(x, y, frame) -> colour, defined at ground-truth (output) resolution. Everything is a pure
// function of its arguments, so every frame is exactly reproducible.
class Sequence {
public:
    struct Rgb {
        double r = 0, g = 0, b = 0;
    };
    using EvalFn = std::function<Rgb(double x, double y, int frame)>;

    // Returns null for an unknown name. frames >= 1; ss = supersampling factor per axis for the ground truth.
    static std::unique_ptr<Sequence> Create(const std::string& name, uint32_t gtW, uint32_t gtH, int frames, int ss);
    // Test hook: a pure translation with the given velocity (ground-truth pixels per frame).
    static std::unique_ptr<Sequence> CreatePan(uint32_t gtW, uint32_t gtH, int frames, int ss, double vx, double vy);

    const std::string& Name() const { return name_; }
    uint32_t W() const { return w_; }
    uint32_t H() const { return h_; }
    int Frames() const { return frames_; }
    int CutFrame() const { return cut_; }                // first frame of the new scene, or -1
    bool IsStatic() const { return static_; }            // every frame identical
    const std::vector<HudRect>& Hud() const { return hud_; } // empty if the sequence has no overlay

    // Ground truth: ss x ss regular supersampling at (w x h) = GT size, quantised to 8-bit.
    Image GroundTruth(int frame) const;
    // Game-resolution frame by point sampling the continuous scene at input pixel centres (aliased, as an engine
    // without anti-aliasing renders it).
    Image InputPoint(int frame, uint32_t iw, uint32_t ih) const;
    // Game-resolution frame by area averaging the ground truth (anti-aliased).
    static Image InputArea(const Image& gt, uint32_t iw, uint32_t ih);

private:
    std::string name_;
    uint32_t w_ = 0, h_ = 0;
    int frames_ = 0, cut_ = -1, ss_ = 3;
    bool static_ = false;
    std::vector<HudRect> hud_;
    EvalFn eval_;
};

const std::vector<std::string>& SequenceNames();

struct SeqScore {
    double psnr = 0;     // luma PSNR vs ground truth, dB
    double ssim = 0;     // luma SSIM (11x11 Gaussian), whole frame
    double flicker = kNaN; // mean |e_t - e_{t-1}| of the luma error e = out - gt, 8-bit levels; NaN for the first frame
    bool hasHud = false;
    double hudPsnr = 0, hudSsim = 0, hudFlicker = kNaN; // inside the overlay rectangles only
};

// Scores consecutive output frames of one run against their ground-truth luma planes. Holds the previous error plane.
class SeqScorer {
public:
    SeqScorer(uint32_t w, uint32_t h, const std::vector<HudRect>& hud);
    SeqScore Add(const std::vector<float>& gtLuma, const Image& out);

private:
    uint32_t w_, h_;
    std::vector<HudRect> hud_;
    std::vector<float> prevErr_;
};

struct MethodSpec {
    std::string label;
    bool alsr = false;
    moonsr::ScalerKind kind = moonsr::ScalerKind::Lanczos;
    float sharpness = 0.0f;
};

struct RunOut {
    std::vector<SeqScore> scores;
    std::vector<double> msMain;   // spatial: ScalePass::Render; ALSR: AlsrUpscaler::Render. NaN if the query was disjoint
    std::vector<double> msMotion; // ALSR only: FrameGenerator::PushFrame (the motion estimator)
    uint64_t outHash = 0;         // FNV-1a over all output frames (determinism check)
    Image keptFrame;              // output at frame keepFrame, if requested
};

// Runs one method over a prepared sequence on one GPU. `inputs[t]` is the game-resolution frame, gtLuma[t] the ground
// truth luma plane. The output size is the ground-truth size. Fresh generator/upscaler state per call.
bool TimeMethod(GpuRunner& gpu, const Sequence& seq, const std::vector<Image>& inputs, const MethodSpec& method, RunOut& out, std::string& err);
bool RunMethod(GpuRunner& gpu, const Sequence& seq, const std::vector<Image>& inputs,
               const std::vector<std::vector<float>>& gtLuma, const MethodSpec& method, int keepFrame, RunOut& out,
               std::string& err);

struct TemporalOptions {
    std::vector<std::string> seqs;      // empty = all
    std::vector<std::string> modes{"aliased", "area"};
    std::vector<std::string> methods;   // substrings; empty = lanczos, spatial, MoonSR temporal ("alsr" also selects it)
    std::vector<double> scales{1.5, 2.0};
    std::vector<float> sharpness{0.0f};
    uint32_t gtW = 1920, gtH = 1080;
    int frames = 64, warmup = 8, ss = 3;
    bool saveImages = false;
    int saveFrame = 40;
    std::filesystem::path outDir;
    std::string tag;
};

int RunTemporalBench(const TemporalOptions& o, const std::vector<std::unique_ptr<GpuRunner>>& gpus, const SystemInfo& si,
                     const std::string& tag);

// --selftest coverage for the temporal mode; returns the number of failed checks (prints PASS/FAIL lines).
int RunTemporalSelfTest(GpuRunner* gpu);

} // namespace bench
