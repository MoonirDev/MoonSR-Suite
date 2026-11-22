#pragma once
#include "common.h"

#include <string>

namespace bench {

// PNG IO through Windows Imaging Component. Read converts to 8-bit BGRA (alpha forced to 255). Write stores 24-bit RGB.
// COM must be initialised on the calling thread.
bool ReadPng(const std::wstring& path, Image& out, std::string& err);
bool WritePng(const std::wstring& path, const Image& img, std::string& err);

// Area-averaging (exact box filter with fractional pixel coverage) resize to (dw, dh), computed in float on the
// 8-bit values as stored (no linearisation), rounded to nearest at the end. Works for any ratio.
Image AreaDownsample(const Image& src, uint32_t dw, uint32_t dh);

// CPU bilinear reference upscale. Sample position of destination pixel x is (x+0.5)*sw/dw - 0.5 + shift (source
// texel units, texel centres at integer indices), clamped to the edge. shift=0 is the correct centre-aligned mapping;
// non-zero values produce deliberately misaligned references that prove the alignment check can see an offset.
Image BilinearUpscale(const Image& src, uint32_t dw, uint32_t dh, double shiftSrcTexels = 0.0);

// Number of input pixels for a ground-truth extent and scale factor: round(gt / scale), at least 1.
uint32_t ScaledExtent(uint32_t gt, double scale);

} // namespace bench
