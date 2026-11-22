// moonsr_fgbench shared declarations.
#pragma once

#include "framegen/frame_generator.h"
#include "gpu/gpu_device.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace fgb {

// 8-bit BGRA image (GPU upload / PNG layout). Alpha is 255.
struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;
    void Alloc(uint32_t width, uint32_t height) {
        w = width;
        h = height;
        px.assign(static_cast<size_t>(w) * h * 4, 255);
    }
    bool Empty() const { return w == 0 || h == 0; }
};

// Binary mask, 1 byte per pixel (0 / 1).
struct Mask {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> m;
};

void ParallelFor(uint32_t n, const std::function<void(uint32_t)>& fn);
std::string Narrow(const std::wstring& w);

// ---- synthetic sequences (synth.cpp) ----
struct Rgb {
    double r = 0, g = 0, b = 0;
};

struct Sequence {
    std::string name;
    std::string description;
    // Colour of the continuous scene at pixel position (x, y) (pixel centres at +0.5) and time t (A = 0, B = 1).
    std::function<Rgb(double x, double y, double t)> scene;
    // Optional: overlay coverage 0..1 at (x, y) (static HUD), used to build the HUD evaluation mask.
    std::function<double(double x, double y)> hudCoverage;
    bool sceneCut = false; // A and B unrelated: no ground-truth intermediate exists
    // Samples per pixel axis when rendering (3 = supersampled box filter, anti-aliased; 1 = point sampled at the pixel
    // centre, i.e. aliased like a game without anti-aliasing).
    uint32_t samples = 3;
};

std::vector<Sequence> MakeSequences(uint32_t w, uint32_t h);
// Supersampled render (n x n stratified samples per pixel, box filter), gamma-encoded values as defined by the scene.
// n = 0: the sequence's own sample count.
Image Render(const Sequence& s, uint32_t w, uint32_t h, double t, uint32_t n = 0);
Mask RenderHudMask(const Sequence& s, uint32_t w, uint32_t h);
// Random-texture image for self-tests (deterministic).
Image NoiseImage(uint32_t w, uint32_t h, uint32_t seed, int shiftX = 0, int shiftY = 0);
// Sequences built from a real captured frame (texture source): camera turn / rotation / layered sprites with the
// source's own HUD pasted static. Real image content, synthetic (exactly known) motion. Names start with "real_".
std::vector<Sequence> MakeRealSequences(const Image& source, uint32_t w, uint32_t h);

// ---- metrics / IO (util.cpp) ----
constexpr double kInf = std::numeric_limits<double>::infinity();
struct Quality {
    double psnr = 0;      // RGB, whole frame, peak 255
    double psnrInner = 0; // RGB, excluding a border band (content entering the frame has no source)
    double ssim = 0;      // luma, Gaussian 11x11 sigma 1.5, whole frame (valid region)
    double psnrMask = 0;  // RGB over a mask (HUD), 0 if no mask
    double psnrEdge = 0;  // RGB over edge pixels of the reference (luma gradient |dx|+|dy| >= 48, inner region)
};
Quality Compare(const Image& ref, const Image& test, uint32_t border, const Mask* mask);
double MaxAbsDiff(const Image& a, const Image& b, uint32_t border = 0);
Image RepeatNearest(const Image& a, const Image& b, double t);
Image LinearBlend(const Image& a, const Image& b, double t);
bool WritePng(const std::wstring& path, const Image& img, std::string& err);
bool ReadPng(const std::wstring& path, Image& img, std::string& err);

// ---- GPU runner (runner.cpp) ----
struct TimingResult {
    uint32_t samples = 0;
    uint32_t discardedBatches = 0;
    double meanMs = 0, medianMs = 0, minMs = 0, p95Ms = 0;
};

class Runner {
public:
    static std::unique_ptr<Runner> Open(const moonsr::AdapterInfo& info, std::string& err);
    const moonsr::AdapterInfo& Info() const { return info_; }
    moonsr::FrameGenerator& Fg() { return *fg_; }
    moonsr::GpuDevice& Device() { return *dev_; }

    // Resets the generator, applies settings, pushes the frames in order (the last two form the analysed pair).
    bool PushSequence(const std::vector<const Image*>& frames, const moonsr::FrameGenSettings& s, std::string& err);
    // Interpolates at t into an internal target and reads it back.
    bool InterpolateRead(float t, moonsr::FrameGenDebugView view, Image& out, std::string& err);
    // Blocks until the GPU finished all submitted work (bench only).
    bool WaitIdle();
    // GPU timestamp timing: PushFrame alternating between frames a and b, and Interpolate(0.5).
    bool Time(const Image& a, const Image& b, const moonsr::FrameGenSettings& s, uint32_t warmup, uint32_t iterations,
              TimingResult& push, TimingResult& interp, TimingResult& pair, std::string& err);
    // Checks that the generator left no SRV / UAV / RTV bound. Returns a description of the first violation.
    std::string CheckUnbound();

private:
    bool Upload(const Image& img, Microsoft::WRL::ComPtr<ID3D11Texture2D>& tex, std::string& err);
    bool EnsureTarget(uint32_t w, uint32_t h, std::string& err);

    moonsr::AdapterInfo info_;
    std::unique_ptr<moonsr::GpuDevice> dev_;
    std::unique_ptr<moonsr::FrameGenerator> fg_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> srcs_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dst_, staging_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    uint32_t dstW_ = 0, dstH_ = 0;
};

} // namespace fgb
