// CrossAdapterLink implementation: moves a texture region from a D3D11 device on one GPU to a D3D11 device on another.
//
// Two methods, chosen in Create() by probing the API (never by vendor):
//
// 1. "D3D12 cross-adapter heap" (preferred). D3D11 cannot open D3D12 cross-adapter resources ("There is no mechanism
//    to share cross-adapter resources across D3D API versions", D3D12 shared-heaps documentation), so the copy is
//    bridged through one D3D12 device per adapter. Per transfer v (ring slot = v % 2), split into K horizontal stripes:
//
//      source D3D11 ctx  : [wait srcDone >= v-2] CopySubresourceRegion(rect -> srcTex[slot]) ; signal fromSrc11 = v ;
//                          Flush
//      source copy queue : wait fromSrc11 >= v ; wait dstX >= v-2 ; for each stripe k: srcTex[slot] rows ->
//                          crossBuf[slot] (placed footprint, cross-adapter heap in system memory), signal srcX = x+k+1 ;
//                          signal srcDone = v
//      dest D3D11 ctx    : signal fromDst11 = v ; Flush   (everything the consumers of earlier transfers submitted)
//      dest copy queue   : wait fromDst11 >= v-1 (consumers of slot v-2 done) ; for each stripe k: wait srcX >= x+k+1,
//                          crossBuf[slot] rows -> dstTex[slot] ; signal dstX = v, dstDone = v
//      dest D3D11 ctx    : wait dstDone >= v   (GPU-side; later destination commands see the data)
//
//    srcTex/dstTex are D3D11 textures created with SHARED | SHARED_NTHANDLE and opened by the D3D12 device of the same
//    adapter. All waits are GPU-side; the CPU never waits in steady state (only when more than kMaxInFlight transfers
//    are queued, and on resize / destruction). The copy-queue command lists (per slot and stripe) are recorded once when
//    the resources are created and re-executed every frame, so the frame path allocates nothing. Copy queues are used so
//    that every resource decays to COMMON after each ExecuteCommandLists and implicit promotion covers the copy states:
//    no barriers are needed and the D3D11 side always sees COMMON resources. The cross-adapter resource is a buffer
//    (supported on every D3D12 adapter), not a row-major texture, which is optional (CrossAdapterRowMajorTextureSupported).
//    Ring of 2 slots so frame v+1 can be copied while the destination still reads frame v. Stripes let the destination's
//    PCIe read of stripe k overlap the source's PCIe write of stripe k+1 instead of the two copies running back to back
//    (K = 4 for frames >= 4 MB, 2 for >= 1 MB, else 1; override with MOONSR_XADAPTER_STRIPES=1..8 for tuning).
//    D3D12 devices are per-adapter singletons in a process: if another module created one on the same adapter, the link
//    shares it (it only creates its own queues, lists, fences and resources on it).
//
// 2. "CPU staging" (mandatory fallback, also forced by the environment variable MOONSR_XADAPTER_FORCE_CPU=1, read in
//    Create()). This path DOES read back to the CPU, because no GPU path exists: CopySubresourceRegion(rect -> staging)
//    on the source, Map(READ) (blocks until the copy is done; the runtime flushes), memcpy row by row into a destination
//    staging texture (ring of 2, so the Map(WRITE) does not wait for the previous upload), CopyResource into the
//    returned texture. The returned frame is always the current one.
//
// Written for MoonSR (original code). Design follows the public D3D12 documentation: "Shared heaps" and "Multi-adapter
// systems" (learn.microsoft.com/windows/win32/direct3d12). No sample code was copied.
//
// Threading: pipeline thread only (see the header). The link uses both GpuDevices' immediate contexts, its own D3D12
// devices/queues, and no internal locking.
#include "gpu/cross_adapter.h"

#include "core/log.h"

#include <d3d11_4.h>
#include <d3d12.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace moonsr {

namespace {

constexpr uint32_t kSlots = 2;
constexpr uint32_t kMaxStripes = 8;
constexpr uint64_t kMaxInFlight = 3;  // transfers queued on the GPUs before Transfer waits on the CPU
constexpr DWORD kIdleTimeoutMs = 5000;

bool Failed(HRESULT hr, std::wstring& error, const wchar_t* what) {
    if (SUCCEEDED(hr)) return false;
    error = std::format(L"CrossAdapterLink: {} failed: {}", what, HrToString(hr));
    MSR_LOG_ERROR(L"{}", error);
    return true;
}

bool Fail(std::wstring& error, std::wstring message) {
    error = L"CrossAdapterLink: " + std::move(message);
    MSR_LOG_ERROR(L"{}", error);
    return false;
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) : h_(h) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    void Reset(HANDLE h = nullptr) {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = h;
    }
    HANDLE Get() const { return h_; }
    HANDLE* Put() {
        Reset();
        return &h_;
    }

private:
    HANDLE h_ = nullptr;
};

bool ForceCpuFromEnvironment() {
    wchar_t buf[8]{};
    const DWORD n = GetEnvironmentVariableW(L"MOONSR_XADAPTER_FORCE_CPU", buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n < std::size(buf) && buf[0] == L'1';
}

// Bytes per texel for uncompressed, non-planar, non-packed DXGI formats; 0 for anything else (block-compressed,
// video, 1-bit, 2x1 packed). Ranges follow the numeric DXGI_FORMAT enumeration, which is fixed ABI.
uint32_t BytesPerTexel(DXGI_FORMAT f) {
    if (f >= DXGI_FORMAT_R32G32B32A32_TYPELESS && f <= DXGI_FORMAT_R32G32B32A32_SINT) return 16;
    if (f >= DXGI_FORMAT_R32G32B32_TYPELESS && f <= DXGI_FORMAT_R32G32B32_SINT) return 12;
    if (f >= DXGI_FORMAT_R16G16B16A16_TYPELESS && f <= DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) return 8;
    if (f >= DXGI_FORMAT_R10G10B10A2_TYPELESS && f <= DXGI_FORMAT_X24_TYPELESS_G8_UINT) return 4;
    if (f >= DXGI_FORMAT_R8G8_TYPELESS && f <= DXGI_FORMAT_R16_SINT) return 2;
    if (f >= DXGI_FORMAT_R8_TYPELESS && f <= DXGI_FORMAT_A8_UNORM) return 1;
    if (f == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) return 4;
    if (f == DXGI_FORMAT_B5G6R5_UNORM || f == DXGI_FORMAT_B5G5R5A1_UNORM || f == DXGI_FORMAT_B4G4R4A4_UNORM) return 2;
    if (f >= DXGI_FORMAT_B8G8R8A8_UNORM && f <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) return 4;
    return 0;
}

bool IsTypeless(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return true;
    default:
        return false;
    }
}

// Creates the texture the link returns on the destination device: SHADER_RESOURCE, plus RENDER_TARGET when the
// format allows it (typed formats: queried; typeless formats: attempted, since views supply the type).
HRESULT CreateOutputTexture(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format, UINT misc,
                            ComPtr<ID3D11Texture2D>& tex) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.MiscFlags = misc;
    UINT support = 0;
    const bool rtv = IsTypeless(format) || (SUCCEEDED(device->CheckFormatSupport(format, &support)) &&
                                            (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET));
    if (rtv) {
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(device->CreateTexture2D(&d, nullptr, tex.ReleaseAndGetAddressOf()))) return S_OK;
    }
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return device->CreateTexture2D(&d, nullptr, tex.ReleaseAndGetAddressOf());
}

// Validates the source texture and rectangle; fills `box` and `desc`.
bool ValidateSource(ID3D11Device* srcDevice, ID3D11Texture2D* texture, const RectI& rect, D3D11_TEXTURE2D_DESC& desc,
                    D3D11_BOX& box, std::wstring& error) {
    if (!texture) return Fail(error, L"Transfer: null texture");
    {
        ComPtr<ID3D11Device> owner;
        texture->GetDevice(&owner);
        if (owner.Get() != static_cast<ID3D11Device*>(srcDevice))
            return Fail(error, L"Transfer: texture does not belong to the source device");
    }
    texture->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1) return Fail(error, L"Transfer: multisampled textures are not supported");
    if (BytesPerTexel(desc.Format) == 0)
        return Fail(error, std::format(L"Transfer: unsupported format {}", static_cast<int>(desc.Format)));
    if (rect.Empty() || rect.left < 0 || rect.top < 0 || static_cast<uint32_t>(rect.right) > desc.Width ||
        static_cast<uint32_t>(rect.bottom) > desc.Height)
        return Fail(error, std::format(L"Transfer: rect ({},{})-({},{}) outside the {}x{} texture", rect.left, rect.top,
                                       rect.right, rect.bottom, desc.Width, desc.Height));
    box = {static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0, static_cast<UINT>(rect.right),
           static_cast<UINT>(rect.bottom), 1};
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// CPU staging path.
class CpuStagingPath {
public:
    CpuStagingPath(GpuDevice& src, GpuDevice& dst) : src_(src), dst_(dst) {}

    ID3D11Texture2D* Transfer(ID3D11Texture2D* texture, const D3D11_BOX& box, DXGI_FORMAT format, std::wstring& error) {
        const uint32_t w = box.right - box.left, h = box.bottom - box.top;
        if (!srcStaging_ || w != width_ || h != height_ || format != format_) {
            if (!Recreate(w, h, format, error)) return nullptr;
        }
        const uint32_t slot = slot_;
        slot_ = (slot_ + 1) % kSlots;

        ID3D11DeviceContext* sc = src_.Context();
        ID3D11DeviceContext* dc = dst_.Context();
        sc->CopySubresourceRegion(srcStaging_.Get(), 0, 0, 0, 0, texture, 0, &box);

        D3D11_MAPPED_SUBRESOURCE ms{}, md{};
        if (Failed(sc->Map(srcStaging_.Get(), 0, D3D11_MAP_READ, 0, &ms), error, L"Map(source staging)")) return nullptr;
        HRESULT hr = dc->Map(dstStaging_[slot].Get(), 0, D3D11_MAP_WRITE, 0, &md);
        if (FAILED(hr)) {
            sc->Unmap(srcStaging_.Get(), 0);
            Failed(hr, error, L"Map(destination staging)");
            return nullptr;
        }
        const size_t rowBytes = static_cast<size_t>(w) * BytesPerTexel(format);
        const auto* s = static_cast<const uint8_t*>(ms.pData);
        auto* d = static_cast<uint8_t*>(md.pData);
        if (ms.RowPitch == md.RowPitch) {
            std::memcpy(d, s, static_cast<size_t>(ms.RowPitch) * (h - 1) + rowBytes);
        } else {
            for (uint32_t y = 0; y < h; ++y) std::memcpy(d + size_t(y) * md.RowPitch, s + size_t(y) * ms.RowPitch, rowBytes);
        }
        dc->Unmap(dstStaging_[slot].Get(), 0);
        sc->Unmap(srcStaging_.Get(), 0);
        dc->CopyResource(output_[slot].Get(), dstStaging_[slot].Get());
        return output_[slot].Get();
    }

private:
    bool Recreate(uint32_t w, uint32_t h, DXGI_FORMAT format, std::wstring& error) {
        srcStaging_.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> srcStaging;
        if (Failed(src_.Device()->CreateTexture2D(&d, nullptr, &srcStaging), error, L"CreateTexture2D(source staging)"))
            return false;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (Failed(dst_.Device()->CreateTexture2D(&d, nullptr, dstStaging_[i].ReleaseAndGetAddressOf()), error,
                       L"CreateTexture2D(destination staging)") ||
                Failed(CreateOutputTexture(dst_.Device(), w, h, format, 0, output_[i]), error,
                       L"CreateTexture2D(destination output)"))
                return false;
        }
        srcStaging_ = std::move(srcStaging);
        width_ = w;
        height_ = h;
        format_ = format;
        slot_ = 0;
        MSR_LOG_INFO(L"CrossAdapterLink: CPU staging resources {}x{} format {}", w, h, static_cast<int>(format));
        return true;
    }

    GpuDevice& src_;
    GpuDevice& dst_;
    ComPtr<ID3D11Texture2D> srcStaging_;
    ComPtr<ID3D11Texture2D> dstStaging_[kSlots];
    ComPtr<ID3D11Texture2D> output_[kSlots];
    uint32_t width_ = 0, height_ = 0, slot_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

// ---------------------------------------------------------------------------------------------------------------------
// D3D12 cross-adapter heap path.
struct D3D12Side {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;  // COPY
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> lists[kSlots][kMaxStripes];
};

// Stripe count override for tuning: MOONSR_XADAPTER_STRIPES=1..8 (0 or unset = automatic). Read once per link.
uint32_t StripesFromEnvironment() {
    wchar_t buf[8]{};
    const DWORD n = GetEnvironmentVariableW(L"MOONSR_XADAPTER_STRIPES", buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return 0;
    const int v = _wtoi(buf);
    return v >= 1 && v <= static_cast<int>(kMaxStripes) ? static_cast<uint32_t>(v) : 0;
}

class D3D12CrossAdapterPath {
public:
    D3D12CrossAdapterPath(GpuDevice& src, GpuDevice& dst) : src_(src), dst_(dst) {}
    ~D3D12CrossAdapterPath() { WaitIdle(); }
    D3D12CrossAdapterPath(const D3D12CrossAdapterPath&) = delete;
    D3D12CrossAdapterPath& operator=(const D3D12CrossAdapterPath&) = delete;

    // Creates the D3D12 devices, queues, lists and fences, and proves that a cross-adapter heap created on the source
    // GPU can be opened on the destination GPU. False (with `error`) means: use the CPU path.
    bool Init(std::wstring& error) {
        if (Failed(src_.Device()->QueryInterface(IID_PPV_ARGS(&srcDev5_)), error, L"source QI(ID3D11Device5)") ||
            Failed(dst_.Device()->QueryInterface(IID_PPV_ARGS(&dstDev5_)), error, L"destination QI(ID3D11Device5)") ||
            Failed(src_.Context()->QueryInterface(IID_PPV_ARGS(&srcCtx4_)), error,
                   L"source QI(ID3D11DeviceContext4)") ||
            Failed(dst_.Context()->QueryInterface(IID_PPV_ARGS(&dstCtx4_)), error,
                   L"destination QI(ID3D11DeviceContext4)"))
            return false;
        if (!InitSide(src_, s_, L"source", error) || !InitSide(dst_, d_, L"destination", error)) return false;

        // Capabilities, for the log. The buffer-based exchange does not depend on them.
        for (auto* side : {&s_, &d_}) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
            if (SUCCEEDED(side->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))))
                MSR_LOG_INFO(L"CrossAdapterLink: {} D3D12 ResourceHeapTier {}, CrossAdapterRowMajorTextureSupported {}, "
                             L"CrossNodeSharingTier {}",
                             side == &s_ ? L"source" : L"destination", static_cast<int>(o.ResourceHeapTier),
                             o.CrossAdapterRowMajorTextureSupported ? 1 : 0, static_cast<int>(o.CrossNodeSharingTier));
        }

        // Fences. Same-adapter D3D12 <-> D3D11 fences are SHARED; D3D12 <-> D3D12 fences are SHARED_CROSS_ADAPTER.
        constexpr auto kShared = D3D12_FENCE_FLAG_SHARED;
        constexpr auto kCross = D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER;
        if (!CreateFence12(s_.device.Get(), kShared, fromSrc11_, L"fromSrc11", error) ||
            !CreateFence12(s_.device.Get(), kShared, srcDone_, L"srcDone", error) ||
            !CreateFence12(s_.device.Get(), kCross, srcX_, L"srcX", error) ||
            !CreateFence12(d_.device.Get(), kCross, dstX_, L"dstX", error) ||
            !CreateFence12(d_.device.Get(), kShared, dstDone_, L"dstDone", error) ||
            !CreateFence12(d_.device.Get(), kShared, fromDst11_, L"fromDst11", error))
            return false;
        if (!OpenFenceIn11(s_.device.Get(), fromSrc11_.Get(), srcDev5_.Get(), fromSrc11On11_, L"fromSrc11", error) ||
            !OpenFenceIn11(s_.device.Get(), srcDone_.Get(), srcDev5_.Get(), srcDoneOn11_, L"srcDone", error) ||
            !OpenFenceIn11(d_.device.Get(), dstDone_.Get(), dstDev5_.Get(), dstDoneOn11_, L"dstDone", error) ||
            !OpenFenceIn11(d_.device.Get(), fromDst11_.Get(), dstDev5_.Get(), fromDst11On11_, L"fromDst11", error) ||
            !OpenFenceIn12(s_.device.Get(), srcX_.Get(), d_.device.Get(), srcXOnD_, L"srcX", error) ||
            !OpenFenceIn12(d_.device.Get(), dstX_.Get(), s_.device.Get(), dstXOnS_, L"dstX", error))
            return false;

        event_.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!event_.Get()) {
            Failed(HRESULT_FROM_WIN32(GetLastError()), error, L"CreateEvent");
            return false;
        }

        // Probe: a small cross-adapter heap on the source, opened on the destination.
        ComPtr<ID3D12Heap> heapS, heapD;
        if (!CreateCrossHeap(64 * 1024, heapS, heapD, error)) return false;
        return true;
    }

    // Returns nullptr on failure. `resourceFailure` is set when the failure happened while (re)creating resources for
    // a new size/format (nothing of this transfer was submitted), so the caller may fall back to the CPU path. A failure
    // while submitting (typically device removal) makes the path unusable; the caller should recreate the link.
    ID3D11Texture2D* Transfer(ID3D11Texture2D* texture, const D3D11_BOX& box, DXGI_FORMAT format, std::wstring& error,
                              bool& resourceFailure) {
        resourceFailure = false;
        if (broken_) {
            Fail(error, L"D3D12 path is unusable after an earlier failure");
            return nullptr;
        }
        const uint32_t w = box.right - box.left, h = box.bottom - box.top;
        if (!srcTex11_[0] || w != width_ || h != height_ || format != format_) {
            if (!Recreate(w, h, format, error)) {
                resourceFailure = true;
                return nullptr;
            }
        }

        const uint64_t v = ++value_;
        const uint32_t slot = static_cast<uint32_t>(v % kSlots);

        // Bound the queue depth (never hit when the GPUs keep up).
        if (v > kMaxInFlight && dstX_->GetCompletedValue() < v - kMaxInFlight &&
            !WaitFence(dstX_.Get(), v - kMaxInFlight, error))
            return Broken();

        // Destination: everything submitted so far (consumers of earlier transfers) is marked by fromDst11 = v.
        if (Failed(dstCtx4_->Signal(fromDst11On11_.Get(), v), error, L"destination ID3D11DeviceContext4::Signal"))
            return Broken();
        dst_.Context()->Flush();

        // Source: copy the rect into the shared slot texture once the source copy queue has finished reading it.
        if (v > kSlots && Failed(srcCtx4_->Wait(srcDoneOn11_.Get(), v - kSlots), error,
                                 L"source ID3D11DeviceContext4::Wait"))
            return Broken();
        src_.Context()->CopySubresourceRegion(srcTex11_[slot].Get(), 0, 0, 0, 0, texture, 0, &box);
        if (Failed(srcCtx4_->Signal(fromSrc11On11_.Get(), v), error, L"source ID3D11DeviceContext4::Signal"))
            return Broken();
        src_.Context()->Flush();

        // Source copy queue: shared texture -> cross-adapter buffer (once the destination has read that slot).
        if (Failed(s_.queue->Wait(fromSrc11_.Get(), v), error, L"source queue Wait") ||
            (v > kSlots && Failed(s_.queue->Wait(dstXOnS_.Get(), v - kSlots), error, L"source queue Wait")))
            return Broken();
        // One list per stripe; srcX counts stripes so the destination can start on stripe k while the source copies
        // stripe k+1 (the two PCIe copies overlap instead of running back to back).
        const uint64_t xBase = xValue_;
        for (uint32_t k = 0; k < stripes_; ++k) {
            ID3D12CommandList* sl[] = {s_.lists[slot][k].Get()};
            s_.queue->ExecuteCommandLists(1, sl);
            if (Failed(s_.queue->Signal(srcX_.Get(), ++xValue_), error, L"source queue Signal")) return Broken();
        }
        if (Failed(s_.queue->Signal(srcDone_.Get(), v), error, L"source queue Signal")) return Broken();

        // Destination copy queue: cross-adapter buffer -> shared output texture (once consumers of v-2 are done).
        if (v > 1 && Failed(d_.queue->Wait(fromDst11_.Get(), v - 1), error, L"destination queue Wait"))
            return Broken();
        for (uint32_t k = 0; k < stripes_; ++k) {
            if (Failed(d_.queue->Wait(srcXOnD_.Get(), xBase + k + 1), error, L"destination queue Wait")) return Broken();
            ID3D12CommandList* dl[] = {d_.lists[slot][k].Get()};
            d_.queue->ExecuteCommandLists(1, dl);
        }
        if (Failed(d_.queue->Signal(dstX_.Get(), v), error, L"destination queue Signal") ||
            Failed(d_.queue->Signal(dstDone_.Get(), v), error, L"destination queue Signal"))
            return Broken();

        // Destination D3D11: later commands wait (GPU-side) for the copy.
        if (Failed(dstCtx4_->Wait(dstDoneOn11_.Get(), v), error, L"destination ID3D11DeviceContext4::Wait"))
            return Broken();
        return dstTex11_[slot].Get();
    }

private:
    bool InitSide(GpuDevice& gpu, D3D12Side& side, const wchar_t* name, std::wstring& error) {
        HRESULT hr = D3D12CreateDevice(gpu.Adapter(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&side.device));
        if (FAILED(hr)) return Fail(error, std::format(L"D3D12CreateDevice on the {} adapter failed: {}", name, HrToString(hr)));
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        if (Failed(side.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&side.queue)), error, L"CreateCommandQueue(COPY)") ||
            Failed(side.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&side.allocator)),
                   error, L"CreateCommandAllocator(COPY)"))
            return false;
        for (auto& perSlot : side.lists) {
            for (auto& list : perSlot) {
                if (Failed(side.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, side.allocator.Get(),
                                                          nullptr, IID_PPV_ARGS(&list)),
                           error, L"CreateCommandList(COPY)") ||
                    Failed(list->Close(), error, L"ID3D12GraphicsCommandList::Close"))
                    return false;
            }
        }
        return true;
    }

    static bool CreateFence12(ID3D12Device* device, D3D12_FENCE_FLAGS flags, ComPtr<ID3D12Fence>& fence,
                              const wchar_t* name, std::wstring& error) {
        const HRESULT hr = device->CreateFence(0, flags, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) return Fail(error, std::format(L"CreateFence({}) failed: {}", name, HrToString(hr)));
        return true;
    }

    static bool OpenFenceIn11(ID3D12Device* owner, ID3D12Fence* fence, ID3D11Device5* device, ComPtr<ID3D11Fence>& out,
                              const wchar_t* name, std::wstring& error) {
        UniqueHandle h;
        HRESULT hr = owner->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, h.Put());
        if (SUCCEEDED(hr)) hr = device->OpenSharedFence(h.Get(), IID_PPV_ARGS(&out));
        if (FAILED(hr)) return Fail(error, std::format(L"sharing fence {} with D3D11 failed: {}", name, HrToString(hr)));
        return true;
    }

    static bool OpenFenceIn12(ID3D12Device* owner, ID3D12Fence* fence, ID3D12Device* device, ComPtr<ID3D12Fence>& out,
                              const wchar_t* name, std::wstring& error) {
        UniqueHandle h;
        HRESULT hr = owner->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, h.Put());
        if (SUCCEEDED(hr)) hr = device->OpenSharedHandle(h.Get(), IID_PPV_ARGS(&out));
        if (FAILED(hr))
            return Fail(error, std::format(L"sharing fence {} across adapters failed: {}", name, HrToString(hr)));
        return true;
    }

    bool CreateCrossHeap(uint64_t bytes, ComPtr<ID3D12Heap>& heapS, ComPtr<ID3D12Heap>& heapD, std::wstring& error) {
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = bytes;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        HRESULT hr = s_.device->CreateHeap(&hd, IID_PPV_ARGS(&heapS));
        if (FAILED(hr)) {
            // Resource heap tier 1 adapters may require the buffers-only restriction.
            hd.Flags |= D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            hr = s_.device->CreateHeap(&hd, IID_PPV_ARGS(&heapS));
        }
        if (FAILED(hr)) return Fail(error, std::format(L"CreateHeap(cross-adapter) failed: {}", HrToString(hr)));
        UniqueHandle h;
        hr = s_.device->CreateSharedHandle(heapS.Get(), nullptr, GENERIC_ALL, nullptr, h.Put());
        if (SUCCEEDED(hr)) hr = d_.device->OpenSharedHandle(h.Get(), IID_PPV_ARGS(&heapD));
        if (FAILED(hr))
            return Fail(error, std::format(L"opening the cross-adapter heap on the destination failed: {}", HrToString(hr)));
        return true;
    }

    bool ShareTexture11To12(ID3D11Texture2D* tex, ID3D12Device* device, ComPtr<ID3D12Resource>& out,
                            std::wstring& error) {
        ComPtr<IDXGIResource1> dxgi;
        if (Failed(tex->QueryInterface(IID_PPV_ARGS(&dxgi)), error, L"QI(IDXGIResource1)")) return false;
        UniqueHandle h;
        if (Failed(dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                            h.Put()),
                   error, L"IDXGIResource1::CreateSharedHandle"))
            return false;
        return !Failed(device->OpenSharedHandle(h.Get(), IID_PPV_ARGS(&out)), error,
                       L"ID3D12Device::OpenSharedHandle(D3D11 texture)");
    }

    bool Recreate(uint32_t w, uint32_t h, DXGI_FORMAT format, std::wstring& error) {
        if (!WaitIdle()) {
            broken_ = true;
            return Fail(error, L"timed out waiting for in-flight transfers before resizing");
        }
        ReleaseResources();

        constexpr UINT kMisc = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = kMisc;
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (Failed(src_.Device()->CreateTexture2D(&td, nullptr, &srcTex11_[i]), error,
                       L"CreateTexture2D(source shared)") ||
                !ShareTexture11To12(srcTex11_[i].Get(), s_.device.Get(), srcTex12_[i], error) ||
                Failed(CreateOutputTexture(dst_.Device(), w, h, format, kMisc, dstTex11_[i]), error,
                       L"CreateTexture2D(destination shared)") ||
                !ShareTexture11To12(dstTex11_[i].Get(), d_.device.Get(), dstTex12_[i], error)) {
                ReleaseResources();
                return false;
            }
        }

        // Linear layout of one frame in the cross-adapter buffer (row pitch aligned to 256 bytes), split into
        // horizontal stripes placed one after another (each offset aligned to 512 bytes).
        const D3D12_RESOURCE_DESC srcDesc = srcTex12_[0]->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT whole{};
        UINT64 total = 0;
        s_.device->GetCopyableFootprints(&srcDesc, 0, 1, 0, &whole, nullptr, nullptr, &total);
        if (total == 0 || total == UINT64_MAX) {
            ReleaseResources();
            return Fail(error, L"GetCopyableFootprints returned no layout");
        }
        const uint64_t rowPitch = whole.Footprint.RowPitch;
        uint32_t stripes = forcedStripes_;
        if (stripes == 0) {
            // Each stripe costs one extra cross-adapter fence hop; only split frames large enough to gain from it.
            const uint64_t bytes = rowPitch * h;
            stripes = bytes >= (4ull << 20) ? 4 : bytes >= (1ull << 20) ? 2 : 1;
        }
        stripes = std::min(stripes, h);
        const uint32_t rowsPerStripe = (h + stripes - 1) / stripes;
        stripes = (h + rowsPerStripe - 1) / rowsPerStripe;
        uint64_t offset = 0;
        for (uint32_t k = 0; k < stripes; ++k) {
            Stripe& st = stripe_[k];
            st.y0 = k * rowsPerStripe;
            st.y1 = std::min(h, st.y0 + rowsPerStripe);
            constexpr uint64_t kPlace = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
            offset = (offset + kPlace - 1) / kPlace * kPlace;
            st.footprint = whole;
            st.footprint.Offset = offset;
            st.footprint.Footprint.Height = st.y1 - st.y0;
            offset += rowPitch * (st.y1 - st.y0);
        }
        stripes_ = stripes;
        constexpr uint64_t kAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        const uint64_t slotBytes = (offset + kAlign - 1) / kAlign * kAlign;
        if (!CreateCrossHeap(slotBytes * kSlots, heapS_, heapD_, error)) {
            ReleaseResources();
            return false;
        }
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = slotBytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (Failed(s_.device->CreatePlacedResource(heapS_.Get(), slotBytes * i, &bd, D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr, IID_PPV_ARGS(&bufS_[i])),
                       error, L"CreatePlacedResource(source cross-adapter buffer)") ||
                Failed(d_.device->CreatePlacedResource(heapD_.Get(), slotBytes * i, &bd, D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr, IID_PPV_ARGS(&bufD_[i])),
                       error, L"CreatePlacedResource(destination cross-adapter buffer)")) {
                ReleaseResources();
                return false;
            }
        }

        // Record the per-slot copies once; they are re-executed every frame.
        if (!RecordLists(error)) {
            ReleaseResources();
            broken_ = true;  // lists may be left open; the caller falls back to the CPU path
            return false;
        }
        width_ = w;
        height_ = h;
        format_ = format;
        MSR_LOG_INFO(L"CrossAdapterLink: D3D12 cross-adapter resources {}x{} format {}, {} bytes per slot, row pitch {}, "
                     L"{} stripe(s)",
                     w, h, static_cast<int>(format), slotBytes, rowPitch, stripes_);
        return true;
    }

    bool RecordLists(std::wstring& error) {
        if (Failed(s_.allocator->Reset(), error, L"source allocator Reset") ||
            Failed(d_.allocator->Reset(), error, L"destination allocator Reset"))
            return false;
        for (uint32_t i = 0; i < kSlots; ++i) {
            for (uint32_t k = 0; k < stripes_; ++k) {
                const Stripe& st = stripe_[k];
                D3D12_TEXTURE_COPY_LOCATION tex{}, buf{};
                tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                tex.SubresourceIndex = 0;
                buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                buf.PlacedFootprint = st.footprint;
                const D3D12_BOX rows{0, st.y0, 0, st.footprint.Footprint.Width, st.y1, 1};

                ID3D12GraphicsCommandList* ls = s_.lists[i][k].Get();
                if (Failed(ls->Reset(s_.allocator.Get(), nullptr), error, L"source list Reset")) return false;
                tex.pResource = srcTex12_[i].Get();
                buf.pResource = bufS_[i].Get();
                ls->CopyTextureRegion(&buf, 0, 0, 0, &tex, &rows);
                if (Failed(ls->Close(), error, L"source list Close")) return false;

                ID3D12GraphicsCommandList* ld = d_.lists[i][k].Get();
                if (Failed(ld->Reset(d_.allocator.Get(), nullptr), error, L"destination list Reset")) return false;
                tex.pResource = dstTex12_[i].Get();
                buf.pResource = bufD_[i].Get();
                ld->CopyTextureRegion(&tex, 0, st.y0, 0, &buf, nullptr);
                if (Failed(ld->Close(), error, L"destination list Close")) return false;
            }
        }
        return true;
    }

    void ReleaseResources() {
        for (uint32_t i = 0; i < kSlots; ++i) {
            srcTex11_[i].Reset();
            srcTex12_[i].Reset();
            dstTex11_[i].Reset();
            dstTex12_[i].Reset();
            bufS_[i].Reset();
            bufD_[i].Reset();
        }
        heapS_.Reset();
        heapD_.Reset();
        width_ = height_ = 0;
        format_ = DXGI_FORMAT_UNKNOWN;
    }

    bool WaitFence(ID3D12Fence* fence, uint64_t value, std::wstring& error) {
        if (fence->GetCompletedValue() >= value) return true;
        HRESULT hr = fence->SetEventOnCompletion(value, event_.Get());
        if (FAILED(hr)) {
            Failed(hr, error, L"ID3D12Fence::SetEventOnCompletion");
            return false;
        }
        if (WaitForSingleObject(event_.Get(), kIdleTimeoutMs) != WAIT_OBJECT_0)
            return Fail(error, std::format(L"fence wait for value {} timed out (completed {})", value,
                                           fence->GetCompletedValue()));
        return true;
    }

    // Waits until both copy queues have finished every submitted transfer. Used before resizing and on destruction.
    bool WaitIdle() {
        if (value_ == 0 || !dstX_ || !srcDone_ || !event_.Get()) return true;
        // A removed device never completes its work; its resources may be released without waiting.
        if (FAILED(src_.DeviceStatus()) || FAILED(dst_.DeviceStatus()) || FAILED(s_.device->GetDeviceRemovedReason()) ||
            FAILED(d_.device->GetDeviceRemovedReason()))
            return true;
        // Make sure the D3D11 signals the queues may be waiting for are submitted.
        src_.Context()->Flush();
        dst_.Context()->Flush();
        std::wstring error;
        return WaitFence(srcDone_.Get(), value_, error) && WaitFence(dstX_.Get(), value_, error);
    }

    ID3D11Texture2D* Broken() {
        broken_ = true;
        return nullptr;
    }

    GpuDevice& src_;
    GpuDevice& dst_;
    ComPtr<ID3D11Device5> srcDev5_, dstDev5_;
    ComPtr<ID3D11DeviceContext4> srcCtx4_, dstCtx4_;
    D3D12Side s_, d_;

    ComPtr<ID3D12Fence> fromSrc11_, srcDone_, srcX_, dstX_, dstDone_, fromDst11_;  // owned by the creating device
    ComPtr<ID3D11Fence> fromSrc11On11_, srcDoneOn11_, dstDoneOn11_, fromDst11On11_;
    ComPtr<ID3D12Fence> srcXOnD_, dstXOnS_;
    UniqueHandle event_;

    ComPtr<ID3D11Texture2D> srcTex11_[kSlots], dstTex11_[kSlots];
    ComPtr<ID3D12Resource> srcTex12_[kSlots], dstTex12_[kSlots], bufS_[kSlots], bufD_[kSlots];
    ComPtr<ID3D12Heap> heapS_, heapD_;
    struct Stripe {
        uint32_t y0 = 0, y1 = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    };
    Stripe stripe_[kMaxStripes];
    uint32_t stripes_ = 1;
    uint32_t forcedStripes_ = StripesFromEnvironment();
    uint64_t xValue_ = 0;  // srcX counts stripes, not transfers

    uint64_t value_ = 0;  // fence value of the last submitted transfer
    uint32_t width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    bool broken_ = false;
};

// ---------------------------------------------------------------------------------------------------------------------
class CrossAdapterLinkImpl final : public CrossAdapterLink {
public:
    CrossAdapterLinkImpl(GpuDevice& src, GpuDevice& dst) : src_(src), dst_(dst), cpu_(src, dst) {}

    // Never fails: the CPU path is always available. Kept as bool for future hard failures.
    bool Init() {
        if (ForceCpuFromEnvironment()) {
            MSR_LOG_INFO(L"CrossAdapterLink: MOONSR_XADAPTER_FORCE_CPU=1, using CPU staging");
            return true;
        }
        auto gpu = std::make_unique<D3D12CrossAdapterPath>(src_, dst_);
        std::wstring why;
        if (gpu->Init(why)) {
            gpu_ = std::move(gpu);
        } else {
            MSR_LOG_WARN(L"CrossAdapterLink: GPU path unavailable ({}), using CPU staging", why);
        }
        return true;
    }

    ID3D11Texture2D* Transfer(ID3D11Texture2D* texture, const RectI& rect, std::wstring& error) override {
        D3D11_TEXTURE2D_DESC desc{};
        D3D11_BOX box{};
        if (!ValidateSource(src_.Device(), texture, rect, desc, box, error)) return nullptr;
        if (gpu_) {
            bool resourceFailure = false;
            ID3D11Texture2D* out = gpu_->Transfer(texture, box, desc.Format, error, resourceFailure);
            if (out || !resourceFailure) return out;
            MSR_LOG_WARN(L"CrossAdapterLink: GPU path cannot handle format {} at {}x{} ({}); switching to CPU staging",
                         static_cast<int>(desc.Format), rect.Width(), rect.Height(), error);
            gpu_.reset();
        }
        return cpu_.Transfer(texture, box, desc.Format, error);
    }

    const wchar_t* Method() const override { return gpu_ ? L"D3D12 cross-adapter heap" : L"CPU staging"; }

private:
    GpuDevice& src_;
    GpuDevice& dst_;
    CpuStagingPath cpu_;
    std::unique_ptr<D3D12CrossAdapterPath> gpu_;
};

} // namespace

std::unique_ptr<CrossAdapterLink> CrossAdapterLink::Create(GpuDevice& source, GpuDevice& destination,
                                                           std::wstring& error) {
    error.clear();
    if (LuidEqual(source.Info().luid, destination.Info().luid) || source.Device() == destination.Device()) {
        Fail(error, L"source and destination are on the same adapter");
        return nullptr;
    }
    auto link = std::make_unique<CrossAdapterLinkImpl>(source, destination);
    if (!link->Init()) return nullptr;
    MSR_LOG_INFO(L"CrossAdapterLink: '{}' -> '{}' using {}", source.Info().name, destination.Info().name, link->Method());
    return link;
}

} // namespace moonsr
