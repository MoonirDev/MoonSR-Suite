// Owner: lead
// moonsr_gpuload: stands in for a game that keeps a GPU fully busy, so MoonSR's behaviour under GPU contention can
// be measured. It runs in its own process at normal GPU priority, like a game, and keeps two batches of long ALU-bound
// compute dispatches queued at all times.
//
//   moonsr_gpuload [--adapter <name substring>] [--seconds <n>] [--batch-ms <target ms per batch>]
//   moonsr_gpuload --probe [--priority] [--iterations <n>] [--adapter ...] [--seconds <n>]   (latency of a tiny dispatch, run beside a load)
//
//   --gpu-class <0..5> sets this process's GPU scheduling class first (experiments; 0 idle .. 5 realtime).
// Test tool only: the shader is compiled at run time with the system d3dcompiler_47.dll, not embedded by fxc.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

#include "engine/gpu_priority.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr char kShader[] = R"(
RWBuffer<uint> output : register(u0);
cbuffer Params : register(b0) { uint iterations; uint3 pad; };
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float a = id.x * 1e-6f, b = 1.0001f, c = 0.9999f;
    [loop] for (uint i = 0; i < iterations; ++i) {
        a = mad(a, b, c); b = mad(b, c, a); c = mad(c, a, b);
        a = frac(a); b = frac(b) + 0.5f; c = frac(c) + 0.5f;
    }
    output[id.x & 65535] = asuint(a + b + c);
}
)";

bool Fail(const char* what, HRESULT hr) {
    std::fprintf(stderr, "%s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
    return false;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring adapterFilter = L"RTX 5060";
    double seconds = 30.0, batchMs = 8.0;
    bool probe = false, priority = false;
    uint32_t fixedIterations = 0;
    int gpuClass = -1;
    for (int i = 1; i < argc; i += 2) {
        const std::wstring_view key = argv[i];
        if (key == L"--probe") { probe = true; --i; continue; }
        if (key == L"--priority") { priority = true; --i; continue; }
        if (i + 1 >= argc) { std::fwprintf(stderr, L"Missing value for %ls\n", argv[i]); return 2; }
        if (key == L"--adapter") adapterFilter = argv[i + 1];
        else if (key == L"--seconds") seconds = _wtof(argv[i + 1]);
        else if (key == L"--batch-ms") batchMs = _wtof(argv[i + 1]);
        else if (key == L"--gpu-class") gpuClass = _wtoi(argv[i + 1]);
        else if (key == L"--iterations") fixedIterations = static_cast<uint32_t>(_wtoi(argv[i + 1]));
        else { std::fwprintf(stderr, L"Unknown option %ls\n", argv[i]); return 2; }
    }

    ComPtr<IDXGIFactory6> factory;
    if (HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)); FAILED(hr)) return Fail("CreateDXGIFactory2", hr), 1;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        adapter->GetDesc1(&desc);
        if (std::wstring_view(desc.Description).find(adapterFilter) != std::wstring_view::npos) break;
        adapter.Reset();
    }
    if (!adapter) { std::fwprintf(stderr, L"No adapter matches '%ls'\n", adapterFilter.c_str()); return 1; }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                       &device, nullptr, &context); FAILED(hr))
        return Fail("D3D11CreateDevice", hr), 1;

    ComPtr<ID3DBlob> code, errors;
    if (HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "gpuload", nullptr, nullptr, "main", "cs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors); FAILED(hr)) {
        if (errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return Fail("D3DCompile", hr), 1;
    }
    ComPtr<ID3D11ComputeShader> shader;
    if (HRESULT hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader); FAILED(hr))
        return Fail("CreateComputeShader", hr), 1;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 65536 * 4;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Buffer> buffer;
    if (HRESULT hr = device->CreateBuffer(&bd, nullptr, &buffer); FAILED(hr)) return Fail("CreateBuffer", hr), 1;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 65536;
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (HRESULT hr = device->CreateUnorderedAccessView(buffer.Get(), &ud, &uav); FAILED(hr)) return Fail("CreateUAV", hr), 1;
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 16;
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> params;
    if (HRESULT hr = device->CreateBuffer(&cd, nullptr, &params); FAILED(hr)) return Fail("CreateBuffer(params)", hr), 1;

    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> fences[2];
    for (auto& f : fences)
        if (HRESULT hr = device->CreateQuery(&qd, &f); FAILED(hr)) return Fail("CreateQuery", hr), 1;

    context->CSSetShader(shader.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[] = {uav.Get()};
    context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* cbs[] = {params.Get()};
    context->CSSetConstantBuffers(0, 1, cbs);

    // One batch = 4 dispatches of 4096 groups (1M threads). Iterations are calibrated so that a batch takes about
    // `batchMs` of GPU time when the GPU is otherwise idle: long enough to be a real competitor, short enough that the
    // GPU scheduler can interleave other work between dispatches.
    uint32_t iterations = 256;
    auto setIterations = [&](uint32_t n) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context->Map(params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const uint32_t data[4] = {n, 0, 0, 0};
            std::memcpy(m.pData, data, sizeof(data));
            context->Unmap(params.Get(), 0);
        }
    };
    auto runBatch = [&](ID3D11Query* fence) {
        for (int d = 0; d < 4; ++d) context->Dispatch(4096, 1, 1);
        context->End(fence);
        context->Flush();
    };
    auto waitFence = [&](ID3D11Query* fence) {
        while (context->GetData(fence, nullptr, 0, 0) == S_FALSE) Sleep(0);
    };
    if (fixedIterations) iterations = fixedIterations;
    for (int round = 0; round < (fixedIterations ? 0 : 6); ++round) {
        setIterations(iterations);
        const auto t0 = std::chrono::steady_clock::now();
        runBatch(fences[0].Get());
        waitFence(fences[0].Get());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 0.05) iterations = static_cast<uint32_t>(std::clamp(iterations * batchMs / ms, 16.0, 1.0e7));
    }
    setIterations(iterations);
    std::fwprintf(stdout, L"gpuload: '%ls', %u iterations per thread, ~%.1f ms per batch, %.0f s\n",
                  desc.Description, iterations, batchMs, seconds);
    std::fflush(stdout);

    if (gpuClass >= 0) {
        // Experiment: set this process's GPU scheduling class directly (0 idle .. 5 realtime).
        using SetFn = LONG(APIENTRY*)(HANDLE, int);
        const auto set = reinterpret_cast<SetFn>(GetProcAddress(GetModuleHandleW(L"gdi32.dll"),
                                                                "D3DKMTSetProcessSchedulingPriorityClass"));
        const LONG status = set ? set(GetCurrentProcess(), gpuClass) : -1;
        std::printf("gpu class %d: status 0x%08lx\n", gpuClass, static_cast<unsigned long>(status));
    }

    if (probe) {
        // Latency probe: one dispatch, then wait for it. Run beside a load, it shows how long a MoonSR-sized pass
        // takes when the GPU is shared. --priority applies MoonSR's policy (engine/gpu_priority.h).
        if (priority) {
            const auto p = moonsr::RaiseGpuPriority(device.Get());
            std::printf("probe priority: process class %ls, device %d\n", moonsr::GpuSchedulingClassName(p.processClass),
                        p.deviceThreadPriority);
        }
        // The job is one dispatch of the calibrated batch (a quarter of --batch-ms of work when the GPU is idle).
        std::vector<double> samples;
        const auto probeEnd = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < probeEnd) {
            const auto t0 = std::chrono::steady_clock::now();
            context->Dispatch(4096, 1, 1);
            context->End(fences[0].Get());
            context->Flush();
            waitFence(fences[0].Get());
            samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            Sleep(2);
        }
        std::sort(samples.begin(), samples.end());
        const size_t n = samples.size();
        std::printf("probe: %zu samples, p50 %.3f ms, p95 %.3f ms, p99 %.3f ms, max %.3f ms\n", n, samples[n / 2],
                    samples[n * 95 / 100], samples[n * 99 / 100], samples.back());
        return 0;
    }

    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    uint64_t batches = 0;
    runBatch(fences[0].Get());
    for (unsigned slot = 1; std::chrono::steady_clock::now() < end; slot ^= 1u, ++batches) {
        runBatch(fences[slot].Get());   // keep two batches queued: the GPU never runs dry
        waitFence(fences[slot ^ 1u].Get());
    }
    waitFence(fences[0].Get());
    waitFence(fences[1].Get());
    std::printf("gpuload: %llu batches done\n", static_cast<unsigned long long>(batches));
    return 0;
}
