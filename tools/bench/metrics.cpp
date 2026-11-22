#include "metrics.h"

#include <cmath>

namespace bench {

namespace {
constexpr double kC1 = (0.01 * 255.0) * (0.01 * 255.0);
constexpr double kC2 = (0.03 * 255.0) * (0.03 * 255.0);
constexpr int kWin = 11;
constexpr int kRad = 5;

inline double SsimTerm(double mx, double my, double exx, double eyy, double exy) {
    const double vx = exx - mx * mx, vy = eyy - my * my, cxy = exy - mx * my;
    return ((2 * mx * my + kC1) * (2 * cxy + kC2)) / ((mx * mx + my * my + kC1) * (vx + vy + kC2));
}
} // namespace

std::vector<float> Luma(const Image& img) {
    std::vector<float> out(static_cast<size_t>(img.w) * img.h);
    for (size_t i = 0; i < out.size(); ++i) {
        const double b = img.px[i * 4 + 0], g = img.px[i * 4 + 1], r = img.px[i * 4 + 2];
        out[i] = static_cast<float>(0.2126 * r + 0.7152 * g + 0.0722 * b);
    }
    return out;
}

double PsnrFromMse(double mse) {
    if (mse <= 0) return kInf;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

std::vector<double> GaussianWindow11() {
    std::vector<double> k(kWin);
    double s = 0;
    for (int i = 0; i < kWin; ++i) {
        const double d = i - kRad;
        k[i] = std::exp(-(d * d) / (2.0 * 1.5 * 1.5));
        s += k[i];
    }
    for (auto& v : k) v /= s;
    return k;
}

double SsimPlanes(const std::vector<float>& x, const std::vector<float>& y, uint32_t w, uint32_t h) {
    if (w < kWin || h < kWin) return 0;
    const auto k = GaussianWindow11();
    const uint32_t ow = w - (kWin - 1), oh = h - (kWin - 1);
    // Horizontal pass: five maps (x, y, xx, yy, xy), each ow x h.
    std::vector<double> m[5];
    for (auto& v : m) v.resize(static_cast<size_t>(ow) * h);
    ParallelFor(h, [&](uint32_t r) {
        const float* px = &x[static_cast<size_t>(r) * w];
        const float* py = &y[static_cast<size_t>(r) * w];
        for (uint32_t c = 0; c < ow; ++c) {
            double a = 0, b = 0, cc = 0, d = 0, e = 0;
            for (int t = 0; t < kWin; ++t) {
                const double vx = px[c + t], vy = py[c + t];
                a += k[t] * vx;
                b += k[t] * vy;
                cc += k[t] * vx * vx;
                d += k[t] * vy * vy;
                e += k[t] * vx * vy;
            }
            const size_t o = static_cast<size_t>(r) * ow + c;
            m[0][o] = a; m[1][o] = b; m[2][o] = cc; m[3][o] = d; m[4][o] = e;
        }
    });
    std::vector<double> rowSum(oh, 0.0);
    ParallelFor(oh, [&](uint32_t r) {
        double acc = 0;
        for (uint32_t c = 0; c < ow; ++c) {
            double q[5] = {0, 0, 0, 0, 0};
            for (int t = 0; t < kWin; ++t) {
                const size_t o = static_cast<size_t>(r + t) * ow + c;
                for (int i = 0; i < 5; ++i) q[i] += k[t] * m[i][o];
            }
            acc += SsimTerm(q[0], q[1], q[2], q[3], q[4]);
        }
        rowSum[r] = acc;
    });
    double total = 0;
    for (double v : rowSum) total += v;
    return total / (static_cast<double>(ow) * oh);
}

double SsimPlanesNaive(const std::vector<float>& x, const std::vector<float>& y, uint32_t w, uint32_t h) {
    const auto k = GaussianWindow11();
    double total = 0;
    uint64_t n = 0;
    for (uint32_t r = kRad; r + kRad < h; ++r)
        for (uint32_t c = kRad; c + kRad < w; ++c) {
            double mx = 0, my = 0;
            for (int j = -kRad; j <= kRad; ++j)
                for (int i = -kRad; i <= kRad; ++i) {
                    const double wgt = k[j + kRad] * k[i + kRad];
                    mx += wgt * x[(r + j) * w + c + i];
                    my += wgt * y[(r + j) * w + c + i];
                }
            double vx = 0, vy = 0, cv = 0;
            for (int j = -kRad; j <= kRad; ++j)
                for (int i = -kRad; i <= kRad; ++i) {
                    const double wgt = k[j + kRad] * k[i + kRad];
                    const double dx = x[(r + j) * w + c + i] - mx, dy = y[(r + j) * w + c + i] - my;
                    vx += wgt * dx * dx;
                    vy += wgt * dy * dy;
                    cv += wgt * dx * dy;
                }
            total += ((2 * mx * my + kC1) * (2 * cv + kC2)) / ((mx * mx + my * my + kC1) * (vx + vy + kC2));
            ++n;
        }
    return total / static_cast<double>(n);
}

Metrics Compare(const Image& ref, const Image& test) {
    Metrics m;
    double se = 0;
    int maxAbs = 0;
    const size_t n = static_cast<size_t>(ref.w) * ref.h;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
            const int d = static_cast<int>(ref.px[i * 4 + c]) - static_cast<int>(test.px[i * 4 + c]);
            se += static_cast<double>(d) * d;
            maxAbs = std::max(maxAbs, std::abs(d));
        }
    m.psnrRgb = PsnrFromMse(se / (3.0 * n));
    m.maxAbs = maxAbs;
    const auto lr = Luma(ref), lt = Luma(test);
    double sl = 0;
    for (size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(lr[i]) - lt[i];
        sl += d * d;
    }
    m.psnrLuma = PsnrFromMse(sl / n);
    m.ssim = SsimPlanes(lr, lt, ref.w, ref.h);
    return m;
}

DiffStats Diff(const Image& a, const Image& b) {
    DiffStats s;
    s.pixels = static_cast<uint64_t>(a.w) * a.h;
    double sum = 0;
    for (size_t i = 0; i < s.pixels; ++i) {
        bool diff = false;
        for (int c = 0; c < 3; ++c) {
            const int d = std::abs(static_cast<int>(a.px[i * 4 + c]) - static_cast<int>(b.px[i * 4 + c]));
            sum += d;
            s.maxAbs = std::max<double>(s.maxAbs, d);
            diff |= d != 0;
        }
        s.differing += diff;
    }
    s.meanAbs = sum / (3.0 * s.pixels);
    return s;
}

} // namespace bench
