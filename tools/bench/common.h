// Shared small types and helpers for moonsr_bench.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace bench {

// 8-bit image, interleaved BGRA (the layout used for GPU upload and PNG conversion). Alpha is always 255.
struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px; // w*h*4, B G R A
    bool Empty() const { return w == 0 || h == 0; }
    void Alloc(uint32_t width, uint32_t height) {
        w = width;
        h = height;
        px.assign(static_cast<size_t>(w) * h * 4, 255);
    }
    uint8_t* Row(uint32_t y) { return px.data() + static_cast<size_t>(y) * w * 4; }
    const uint8_t* Row(uint32_t y) const { return px.data() + static_cast<size_t>(y) * w * 4; }
};

// Runs fn(y) for y in [0, n) on all hardware threads. fn must be thread safe for distinct y.
inline void ParallelFor(uint32_t n, const std::function<void(uint32_t)>& fn) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned nt = std::min<unsigned>(hw, std::max<uint32_t>(1, n));
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; ++t)
        th.emplace_back([=, &fn] {
            for (uint32_t y = t; y < n; y += nt) fn(y);
        });
    for (auto& t : th) t.join();
}

inline uint64_t Fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);

} // namespace bench
