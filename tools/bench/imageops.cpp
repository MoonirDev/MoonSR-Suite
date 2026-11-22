#include "imageops.h"

#include <cmath>

namespace bench {

uint32_t ScaledExtent(uint32_t gt, double scale) {
    return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(gt / scale)));
}

namespace {
struct Tap {
    uint32_t first = 0;
    std::vector<float> w;
};
// Exact area-average weights: destination pixel i covers [i*s, (i+1)*s) in source pixel units (s = sn/dn).
std::vector<Tap> AreaTaps(uint32_t sn, uint32_t dn) {
    std::vector<Tap> taps(dn);
    const double s = static_cast<double>(sn) / dn;
    for (uint32_t i = 0; i < dn; ++i) {
        const double a = i * s, b = (i + 1) * s;
        const uint32_t i0 = static_cast<uint32_t>(std::floor(a + 1e-9));
        const uint32_t i1 = std::min<uint32_t>(sn, static_cast<uint32_t>(std::ceil(b - 1e-9)));
        Tap t;
        t.first = i0;
        double sum = 0;
        for (uint32_t k = i0; k < i1; ++k) {
            const double cov = std::min<double>(b, k + 1.0) - std::max<double>(a, static_cast<double>(k));
            t.w.push_back(static_cast<float>(cov));
            sum += cov;
        }
        for (auto& w : t.w) w = static_cast<float>(w / sum);
        taps[i] = std::move(t);
    }
    return taps;
}
inline uint8_t Q8(float v) { return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 255)); }
} // namespace

Image AreaDownsample(const Image& src, uint32_t dw, uint32_t dh) {
    const auto tx = AreaTaps(src.w, dw);
    const auto ty = AreaTaps(src.h, dh);
    // Horizontal pass into float rows (3 channels), then vertical.
    std::vector<float> tmp(static_cast<size_t>(dw) * src.h * 3);
    ParallelFor(src.h, [&](uint32_t y) {
        const uint8_t* r = src.Row(y);
        float* o = tmp.data() + static_cast<size_t>(y) * dw * 3;
        for (uint32_t x = 0; x < dw; ++x) {
            float acc[3] = {0, 0, 0};
            const Tap& t = tx[x];
            for (size_t k = 0; k < t.w.size(); ++k) {
                const uint8_t* p = r + (t.first + k) * 4;
                acc[0] += t.w[k] * p[0];
                acc[1] += t.w[k] * p[1];
                acc[2] += t.w[k] * p[2];
            }
            o[x * 3 + 0] = acc[0];
            o[x * 3 + 1] = acc[1];
            o[x * 3 + 2] = acc[2];
        }
    });
    Image out;
    out.Alloc(dw, dh);
    ParallelFor(dh, [&](uint32_t y) {
        const Tap& t = ty[y];
        uint8_t* o = out.Row(y);
        for (uint32_t x = 0; x < dw; ++x) {
            float acc[3] = {0, 0, 0};
            for (size_t k = 0; k < t.w.size(); ++k) {
                const float* p = tmp.data() + (static_cast<size_t>(t.first + k) * dw + x) * 3;
                acc[0] += t.w[k] * p[0];
                acc[1] += t.w[k] * p[1];
                acc[2] += t.w[k] * p[2];
            }
            o[x * 4 + 0] = Q8(acc[0]);
            o[x * 4 + 1] = Q8(acc[1]);
            o[x * 4 + 2] = Q8(acc[2]);
        }
    });
    return out;
}

Image BilinearUpscale(const Image& src, uint32_t dw, uint32_t dh, double shift) {
    Image out;
    out.Alloc(dw, dh);
    const double sx = static_cast<double>(src.w) / dw, sy = static_cast<double>(src.h) / dh;
    ParallelFor(dh, [&](uint32_t y) {
        const double fy = std::clamp((y + 0.5) * sy - 0.5 + shift, 0.0, static_cast<double>(src.h - 1));
        const uint32_t y0 = static_cast<uint32_t>(std::floor(fy));
        const uint32_t y1 = std::min(y0 + 1, src.h - 1);
        const double wy = fy - y0;
        for (uint32_t x = 0; x < dw; ++x) {
            const double fx = std::clamp((x + 0.5) * sx - 0.5 + shift, 0.0, static_cast<double>(src.w - 1));
            const uint32_t x0 = static_cast<uint32_t>(std::floor(fx));
            const uint32_t x1 = std::min(x0 + 1, src.w - 1);
            const double wx = fx - x0;
            for (int c = 0; c < 3; ++c) {
                const double a = src.Row(y0)[x0 * 4 + c] * (1 - wx) + src.Row(y0)[x1 * 4 + c] * wx;
                const double b = src.Row(y1)[x0 * 4 + c] * (1 - wx) + src.Row(y1)[x1 * 4 + c] * wx;
                out.Row(y)[x * 4 + c] = Q8(static_cast<float>(a * (1 - wy) + b * wy));
            }
        }
    });
    return out;
}

} // namespace bench
