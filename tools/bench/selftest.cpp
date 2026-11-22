// --selftest: validates the metric implementations and the image/GPU plumbing against analytically known answers and
// against an independent direct implementation. Exits non-zero (via the failure count) if any check fails.
#include "gpu_runner.h"
#include "imageops.h"
#include "metrics.h"

#include <windows.h>

#include <cmath>
#include <filesystem>

namespace bench {

namespace {
int g_fail = 0;

void Check(bool ok, const char* name, const char* detail = "") {
    printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name, *detail ? " : " : "", detail);
    if (!ok) ++g_fail;
}
std::string F(const char* fmt, double a, double b = 0, double c = 0) {
    char buf[200];
    snprintf(buf, sizeof(buf), fmt, a, b, c);
    return buf;
}

uint32_t Rng(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}
Image RandomImage(uint32_t w, uint32_t h, uint32_t seed, int lo = 0, int hi = 255) {
    Image img;
    img.Alloc(w, h);
    uint32_t s = seed;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
        for (int c = 0; c < 3; ++c) img.px[i * 4 + c] = static_cast<uint8_t>(lo + Rng(s) % (hi - lo + 1));
    return img;
}
Image Constant(uint32_t w, uint32_t h, int v) {
    Image img;
    img.Alloc(w, h);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) img.px[i * 4 + 0] = img.px[i * 4 + 1] = img.px[i * 4 + 2] = static_cast<uint8_t>(v);
    return img;
}
} // namespace

int RunSelfTest(GpuRunner* gpu) {
    g_fail = 0;
    printf("== moonsr_bench self-test ==\n");

    // 1. Gaussian window.
    {
        const auto k = GaussianWindow11();
        double sum = 0;
        for (double v : k) sum += v;
        Check(std::abs(sum - 1.0) < 1e-12, "Gaussian window sums to 1", F("sum=%.15f", sum).c_str());
        // Independent hand value: centre tap = 1 / (1 + 2*sum_{d=1..5} exp(-d^2/4.5)) = 0.266012...
        Check(std::abs(k[5] - 0.266012) < 1e-6 && std::abs(k[0] - k[10]) < 1e-15, "Gaussian window centre tap and symmetry",
              F("centre=%.6f (expected 0.266012)", k[5]).c_str());
    }

    // 2. Identical images.
    {
        const Image a = RandomImage(64, 48, 1);
        const Metrics m = Compare(a, a);
        Check(std::isinf(m.psnrRgb) && std::isinf(m.psnrLuma), "identical images: PSNR (RGB and luma) is +inf");
        Check(std::abs(m.ssim - 1.0) < 1e-12 && m.maxAbs == 0, "identical images: SSIM == 1", F("ssim=%.15f", m.ssim).c_str());
    }

    // 3. Constant offset of 10 levels on every channel: MSE = 100, PSNR = 20 log10(255/10).
    {
        Image a = RandomImage(64, 48, 2, 20, 200), b = a;
        for (size_t i = 0; i < static_cast<size_t>(a.w) * a.h; ++i)
            for (int c = 0; c < 3; ++c) b.px[i * 4 + c] += 10;
        const Metrics m = Compare(a, b);
        const double expect = 20.0 * std::log10(255.0 / 10.0);
        Check(std::abs(m.psnrRgb - expect) < 1e-9, "constant offset 10: PSNR RGB equals analytic value",
              F("got %.9f dB, expected %.9f dB", m.psnrRgb, expect).c_str());
        Check(std::abs(m.psnrLuma - expect) < 1e-4, "constant offset 10: PSNR luma equals analytic value (float luma)",
              F("got %.6f dB, expected %.6f dB", m.psnrLuma, expect).c_str());
    }

    // 4. +-7 alternating error: MSE = 49 exactly.
    {
        Image a = RandomImage(64, 48, 3, 20, 200), b = a;
        for (uint32_t y = 0; y < a.h; ++y)
            for (uint32_t x = 0; x < a.w; ++x)
                for (int c = 0; c < 3; ++c) b.Row(y)[x * 4 + c] = static_cast<uint8_t>(a.Row(y)[x * 4 + c] + (((x + y) & 1) ? 7 : -7));
        const Metrics m = Compare(a, b);
        const double expect = 20.0 * std::log10(255.0 / 7.0);
        Check(std::abs(m.psnrRgb - expect) < 1e-9, "alternating +-7 error: PSNR RGB equals analytic value",
              F("got %.9f dB, expected %.9f dB", m.psnrRgb, expect).c_str());
    }

    // 5. SSIM of two constant images has a closed form: (2 mx my + C1)/(mx^2 + my^2 + C1) (variance terms cancel).
    {
        const Image a = Constant(40, 40, 100), b = Constant(40, 40, 110);
        const double c1 = (0.01 * 255) * (0.01 * 255);
        const double expect = (2.0 * 100 * 110 + c1) / (100.0 * 100 + 110.0 * 110 + c1);
        const double got = SsimPlanes(Luma(a), Luma(b), 40, 40);
        Check(std::abs(got - expect) < 1e-9, "SSIM of constant images equals closed form",
              F("got %.12f, expected %.12f", got, expect).c_str());
    }

    // 6. Separable SSIM vs direct 2-D implementation on textured pairs.
    {
        const Image a = RandomImage(61, 47, 4);
        Image b = RandomImage(61, 47, 5);
        Image noisy = a;
        uint32_t s = 99;
        for (size_t i = 0; i < static_cast<size_t>(a.w) * a.h; ++i)
            for (int c = 0; c < 3; ++c) noisy.px[i * 4 + c] = static_cast<uint8_t>(std::clamp<int>(a.px[i * 4 + c] + static_cast<int>(Rng(s) % 41) - 20, 0, 255));
        Image half = a;
        for (size_t i = 0; i < static_cast<size_t>(a.w) * a.h; ++i)
            for (int c = 0; c < 3; ++c) half.px[i * 4 + c] = a.px[i * 4 + c] / 2;
        double worst = 0;
        for (const Image* y : {&b, &noisy, &half}) {
            const double f = SsimPlanes(Luma(a), Luma(*y), a.w, a.h), n = SsimPlanesNaive(Luma(a), Luma(*y), a.w, a.h);
            worst = std::max(worst, std::abs(f - n));
        }
        Check(worst < 1e-9, "separable SSIM matches independent direct 11x11 implementation (3 pairs)", F("max |diff| = %.3e", worst).c_str());
        const double sn = SsimPlanes(Luma(a), Luma(noisy), a.w, a.h), su = SsimPlanes(Luma(a), Luma(b), a.w, a.h);
        const double s1 = SsimPlanes(Luma(a), Luma(noisy), a.w, a.h), s2 = SsimPlanes(Luma(noisy), Luma(a), a.w, a.h);
        Check(sn < 1.0 && sn > su && std::abs(s1 - s2) < 1e-12, "SSIM ordering (noisy > unrelated, < 1) and symmetry",
              F("noisy=%.4f unrelated=%.4f", sn, su).c_str());
    }

    // 7. Area downsample.
    {
        Image blocks;
        blocks.Alloc(4, 4);
        const int v[4] = {10, 30, 50, 90};
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x) blocks.Row(y)[x * 4] = blocks.Row(y)[x * 4 + 1] = blocks.Row(y)[x * 4 + 2] = static_cast<uint8_t>(y < 2 ? v[x] : v[3 - x]);
        const Image d = AreaDownsample(blocks, 2, 2);
        // top-left block: rows 0,1 = (10,30) -> 20 ; top-right (50,90) -> 70 ; bottom-left (90,50) -> 70 ; bottom-right (30,10) -> 20
        Check(d.Row(0)[0] == 20 && d.Row(0)[4] == 70 && d.Row(1)[0] == 70 && d.Row(1)[4] == 20, "2x area downsample equals 2x2 block mean");
        Image row;
        row.Alloc(3, 3);
        for (uint32_t y = 0; y < 3; ++y)
            for (uint32_t x = 0; x < 3; ++x) row.Row(y)[x * 4] = row.Row(y)[x * 4 + 1] = row.Row(y)[x * 4 + 2] = static_cast<uint8_t>(x * 90);
        const Image d2 = AreaDownsample(row, 2, 2); // 1.5x: (0 + 0.5*90)/1.5 = 30 ; (0.5*90 + 180)/1.5 = 150
        Check(d2.Row(0)[0] == 30 && d2.Row(0)[4] == 150, "1.5x area downsample equals fractional-coverage average",
              F("got %.0f, %.0f (expected 30, 150)", d2.Row(0)[0], d2.Row(0)[4]).c_str());
        const Image k = AreaDownsample(Constant(31, 17, 77), 10, 6);
        bool allSame = true;
        for (size_t i = 0; i < static_cast<size_t>(k.w) * k.h; ++i) allSame &= k.px[i * 4] == 77;
        Check(allSame, "area downsample preserves a constant image (non-integer ratio)");
    }

    // 8. CPU bilinear reference.
    {
        Image ramp;
        ramp.Alloc(2, 1);
        ramp.Row(0)[0] = ramp.Row(0)[1] = ramp.Row(0)[2] = 0;
        ramp.Row(0)[4] = ramp.Row(0)[5] = ramp.Row(0)[6] = 100;
        const Image u = BilinearUpscale(ramp, 4, 1); // sample positions -0.25 (clamp 0), 0.25, 0.75, 1.25 (clamp 1)
        Check(u.Row(0)[0] == 0 && u.Row(0)[4] == 25 && u.Row(0)[8] == 75 && u.Row(0)[12] == 100, "CPU bilinear 2x ramp matches hand values (0,25,75,100)");
        const Image a = RandomImage(16, 9, 8);
        Check(Diff(BilinearUpscale(a, 16, 9), a).maxAbs == 0, "CPU bilinear at 1x is the identity");
    }

    // 9. PNG round trip.
    {
        const Image a = RandomImage(37, 23, 9);
        const auto path = std::filesystem::temp_directory_path() / "moonsr_bench_selftest.png";
        std::string err;
        Image b;
        const bool ok = WritePng(path.wstring(), a, err) && ReadPng(path.wstring(), b, err);
        Check(ok && b.w == a.w && b.h == a.h && a.px == b.px, "PNG write/read round trip is lossless", err.c_str());
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    // 10. GPU plumbing (needs an adapter).
    if (gpu) {
        printf("-- GPU checks on: %s\n", Narrow(gpu->Info().name).c_str());
        const Image a = RandomImage(8, 6, 10);
        Image up;
        std::string err;
        if (!gpu->Upscale(a, 16, 12, moonsr::ScalerKind::Nearest, 0.0f, up, err)) {
            Check(false, "GPU nearest 2x upscale ran", err.c_str());
        } else {
            Image ref;
            ref.Alloc(16, 12);
            for (uint32_t y = 0; y < 12; ++y)
                for (uint32_t x = 0; x < 16; ++x) memcpy(ref.Row(y) + x * 4, a.Row(y / 2) + (x / 2) * 4, 4);
            const DiffStats d = Diff(ref, up);
            Check(d.maxAbs == 0, "GPU nearest 2x == exact pixel replication (upload, orientation, channel order, readback)",
                  F("max abs diff = %.0f", d.maxAbs).c_str());
        }
        if (gpu->Upscale(a, 32, 24, moonsr::ScalerKind::Bilinear, 0.0f, up, err)) {
            const DiffStats d = Diff(BilinearUpscale(a, 32, 24), up);
            Check(d.maxAbs <= 2, "GPU bilinear 4x within 2 levels of the CPU centre-aligned reference",
                  F("max abs diff = %.0f, mean abs = %.4f", d.maxAbs, d.meanAbs).c_str());
        } else {
            Check(false, "GPU bilinear 4x upscale ran", err.c_str());
        }
    } else {
        printf("-- GPU checks skipped (no adapter selected)\n");
    }

    printf("== %s (%d failure%s) ==\n", g_fail ? "SELF-TEST FAILED" : "self-test passed", g_fail, g_fail == 1 ? "" : "s");
    return g_fail;
}

} // namespace bench
