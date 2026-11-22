#pragma once
#include "common.h"

#include <limits>

namespace bench {

constexpr double kInf = std::numeric_limits<double>::infinity();

struct Metrics {
    double psnrRgb = 0;  // dB, MSE over all three channels, peak 255; +inf for identical images
    double psnrLuma = 0; // dB, on BT.709 luma of the gamma-encoded values (float, not rounded), peak 255
    double ssim = 0;     // mean SSIM, 11x11 Gaussian sigma 1.5, K1 0.01, K2 0.03, L 255, on luma, valid region only
    double maxAbs = 0;   // max |a-b| over RGB channels, 8-bit levels
};

// BT.709 luma weights applied to the stored (gamma-encoded) values; result in 0..255, float.
std::vector<float> Luma(const Image& img);

double PsnrFromMse(double mse);

// Images must have identical dimensions (checked by the caller).
Metrics Compare(const Image& ref, const Image& test);

// Mean SSIM on two equally sized float planes (values 0..255). Separable Gaussian implementation.
double SsimPlanes(const std::vector<float>& x, const std::vector<float>& y, uint32_t w, uint32_t h);
// Straightforward O(w*h*121) direct implementation used only by --selftest as an independent cross-check.
double SsimPlanesNaive(const std::vector<float>& x, const std::vector<float>& y, uint32_t w, uint32_t h);

// The 11 normalised Gaussian window taps (sigma 1.5).
std::vector<double> GaussianWindow11();

struct DiffStats {
    double maxAbs = 0;
    double meanAbs = 0;
    uint64_t differing = 0; // pixels where any channel differs
    uint64_t pixels = 0;
};
DiffStats Diff(const Image& a, const Image& b);

} // namespace bench
