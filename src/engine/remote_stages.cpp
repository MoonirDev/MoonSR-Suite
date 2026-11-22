// Owner: lead
#include "engine/remote_stages.h"

#include "core/log.h"
#include "engine/gpu_priority.h"
#include "gpu/cross_adapter.h"
#include "gpu/gpu_device.h"

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <sddl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <format>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <vector>

namespace moonsr {
namespace {
using Microsoft::WRL::ComPtr;

class RemoteFrameGenerator final : public FrameGenerator {
public:
    RemoteFrameGenerator(GpuDevice& main, GpuDevice& remote) : main_(main), remote_(remote) {}

    bool Init(std::wstring& error) {
        inner_ = FrameGenerator::Create(remote_, error);
        if (!inner_) return false;
        up_ = CrossAdapterLink::Create(main_, remote_, error);
        if (!up_) return false;
        down_ = CrossAdapterLink::Create(remote_, main_, error);
        if (!down_) return false;
        MSR_LOG_INFO(L"Frame generation runs on '{}' (main GPU '{}'); frames cross by {}", remote_.Info().name,
                     main_.Info().name, up_->Method());
        return true;
    }

    void SetSettings(const FrameGenSettings& settings) override { inner_->SetSettings(settings); }

    bool PushFrame(ID3D11Texture2D* src, const RectI& srcRect, int64_t qpc) override {
        if (!src || srcRect.Empty()) return false;
        const Size size{static_cast<uint32_t>(srcRect.Width()), static_cast<uint32_t>(srcRect.Height())};
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return Fail(L"frame generation input is not BGRA8");
        if (size != size_ && !EnsureLocal(size)) return false;
        std::wstring error;
        ID3D11Texture2D* remoteFrame = up_->Transfer(src, srcRect, error);
        if (!remoteFrame) return Fail(error);
        if (!inner_->PushFrame(remoteFrame, RectI{0, 0, srcRect.Width(), srcRect.Height()}, qpc)) return false;
        // Keep the real frame on the main device too: RealFrame() must not cross back.
        newest_ ^= 1u;
        const D3D11_BOX box{static_cast<UINT>(srcRect.left), static_cast<UINT>(srcRect.top), 0,
                            static_cast<UINT>(srcRect.right), static_cast<UINT>(srcRect.bottom), 1};
        main_.Context()->CopySubresourceRegion(local_[newest_].Get(), 0, 0, 0, 0, src, 0, &box);
        count_ = std::min(count_ + 1u, 2u);
        return true;
    }

    void Reset() override {
        inner_->Reset();
        count_ = 0;
    }

    FramePairInfo PairInfo() const override { return inner_->PairInfo(); }

    ID3D11Texture2D* RealFrame(uint32_t index) const override {
        if (index >= count_) return nullptr;
        return local_[index == 0 ? newest_ : newest_ ^ 1u].Get();
    }

    Size FrameSize() const override { return size_; }

    bool Interpolate(float t, ID3D11RenderTargetView* dst) override {
        if (!dst || size_.Empty()) return false;
        if ((!gen_ || genSize_ != size_) && !EnsureGenerated()) return false;
        if (!inner_->Interpolate(t, genRtv_.Get())) return false;
        std::wstring error;
        ID3D11Texture2D* back = down_->Transfer(gen_.Get(), RectI{0, 0, static_cast<int32_t>(size_.width),
                                                                   static_cast<int32_t>(size_.height)}, error);
        if (!back) return Fail(error);
        ComPtr<ID3D11Resource> target;
        dst->GetResource(&target);
        const D3D11_BOX box{0, 0, 0, size_.width, size_.height, 1};
        main_.Context()->CopySubresourceRegion(target.Get(), 0, 0, 0, 0, back, 0, &box);
        return true;
    }

    MotionFieldView MotionField() const override { return {}; } // not shared across GPUs (see header)

    void SetDebugView(FrameGenDebugView view) override { inner_->SetDebugView(view); }

private:
    bool Fail(const std::wstring& what) {
        if (!failureLogged_) {
            failureLogged_ = true;
            MSR_LOG_ERROR(L"Frame generation on '{}': {}", remote_.Info().name, what);
        }
        return false;
    }

    bool EnsureLocal(Size size) {
        for (auto& t : local_) t.Reset();
        count_ = 0;
        size_ = {};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size.width;
        td.Height = size.height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        for (auto& t : local_) {
            const HRESULT hr = main_.Device()->CreateTexture2D(&td, nullptr, &t);
            if (FAILED(hr)) return Fail(std::format(L"local real-frame copy {}x{}: {}", size.width, size.height, HrToString(hr)));
        }
        size_ = size;
        return true;
    }

    bool EnsureGenerated() {
        genRtv_.Reset();
        gen_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size_.width;
        td.Height = size_.height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        HRESULT hr = remote_.Device()->CreateTexture2D(&td, nullptr, &gen_);
        if (SUCCEEDED(hr)) hr = remote_.Device()->CreateRenderTargetView(gen_.Get(), nullptr, &genRtv_);
        if (FAILED(hr)) {
            gen_.Reset();
            return Fail(std::format(L"generated-frame target {}x{}: {}", size_.width, size_.height, HrToString(hr)));
        }
        genSize_ = size_;
        return true;
    }

    GpuDevice& main_;
    GpuDevice& remote_;
    std::unique_ptr<FrameGenerator> inner_;
    std::unique_ptr<CrossAdapterLink> up_, down_;
    ComPtr<ID3D11Texture2D> local_[2];
    uint32_t newest_ = 0, count_ = 0;
    Size size_;
    ComPtr<ID3D11Texture2D> gen_;
    ComPtr<ID3D11RenderTargetView> genRtv_;
    Size genSize_;
    bool failureLogged_ = false;
};

class RemoteNeuralBridge final : public NeuralBridge {
public:
    RemoteNeuralBridge(GpuDevice& main, GpuDevice& remote, std::unique_ptr<NeuralBridge> inner)
        : main_(main), remote_(remote), inner_(std::move(inner)) {}

    bool Init(std::wstring& error) {
        up_ = CrossAdapterLink::Create(main_, remote_, error);
        if (!up_) return false;
        guideUp_ = CrossAdapterLink::Create(main_, remote_, error);
        if (!guideUp_) return false;
        down_ = CrossAdapterLink::Create(remote_, main_, error);
        if (!down_) return false;
        MSR_LOG_INFO(L"Neural rendering runs on '{}' (main GPU '{}'); frames cross by {}", remote_.Info().name,
                     main_.Info().name, up_->Method());
        return true;
    }

    bool SupportsLiveSettings() const override { return inner_->SupportsLiveSettings(); }
    bool UpdateSettings(const NeuralSettings& settings, std::wstring& error) override {
        return inner_->UpdateSettings(settings, error);
    }

    bool Process(ID3D11Texture2D* texture, const RectI& rect, int64_t captureQpc, uint64_t sequence, bool reset,
                 const PictureSettings& picture, ID3D11Texture2D*& enhanced, RectI& enhancedRect,
                 std::wstring& error) override {
        return ProcessWithGuide(texture, rect, nullptr, captureQpc, sequence, reset, picture, enhanced, enhancedRect, error);
    }

    bool ProcessWithGuide(ID3D11Texture2D* texture, const RectI& rect, ID3D11Texture2D* flowGuide, int64_t captureQpc,
                          uint64_t sequence, bool reset, const PictureSettings& picture, ID3D11Texture2D*& enhanced,
                          RectI& enhancedRect, std::wstring& error) override {
        enhanced = nullptr;
        enhancedRect = {};
        ID3D11Texture2D* remoteInput = up_->Transfer(texture, rect, error);
        if (!remoteInput) return false;
        ID3D11Texture2D* remoteGuide = nullptr;
        if (flowGuide) {
            D3D11_TEXTURE2D_DESC gd{};
            flowGuide->GetDesc(&gd);
            remoteGuide = guideUp_->Transfer(flowGuide, RectI{0, 0, static_cast<int32_t>(gd.Width),
                                                             static_cast<int32_t>(gd.Height)}, error);
            if (!remoteGuide) return false;
        }
        ID3D11Texture2D* remoteOut = nullptr;
        RectI remoteRect;
        if (!inner_->ProcessWithGuide(remoteInput, RectI{0, 0, rect.Width(), rect.Height()}, remoteGuide, captureQpc,
                                      sequence, reset, picture, remoteOut, remoteRect, error)) return false;
        ID3D11Texture2D* back = down_->Transfer(remoteOut, remoteRect, error);
        if (!back) return false;
        enhanced = back;
        enhancedRect = {0, 0, remoteRect.Width(), remoteRect.Height()};
        return true;
    }

private:
    GpuDevice& main_;
    GpuDevice& remote_;
    std::unique_ptr<NeuralBridge> inner_;
    std::unique_ptr<CrossAdapterLink> up_, guideUp_, down_;
};

// ---- Hosted neural rendering (see the header) -------------------------------------------------------------------

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void Reset(HANDLE h = nullptr) {
        if (h_) CloseHandle(h_);
        h_ = h == INVALID_HANDLE_VALUE ? nullptr : h;
    }
    HANDLE Get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HANDLE h_ = nullptr;
};

// Pipe protocol between HostedNeuralBridge and NeuralHost. Both ends are built from this file; the version and the
// fixed message sizes guard against a stale MoonSR_NrHost.exe beside a newer executable.
constexpr uint32_t kHostProtocol = 2;
enum class HostOp : uint32_t { Create = 1, Update = 2, Process = 3, Quit = 4 };
struct HostRequest {
    uint32_t protocol = kHostProtocol;
    HostOp op = HostOp::Quit;
    NeuralSettings settings;      // Create, Update
    LUID adapter{};               // Create
    wchar_t modelPath[2048]{};    // Create
    // Handle values valid in the helper process (duplicated into it, owned by it from then on); 0 = none.
    uint64_t toHost = 0, fromHost = 0;          // Create: the shared fences
    uint64_t input = 0, guide = 0, output = 0;  // Process with newTextures = 1
    uint32_t newTextures = 0;
    uint32_t width = 0, height = 0, hasGuide = 0, reset = 0;
    uint64_t fenceValue = 0, sequence = 0;
    int64_t captureQpc = 0;
    PictureSettings picture;
};
struct HostReply {
    uint32_t ok = 0;
    uint32_t liveSettings = 0;
    uint32_t signalled = 0; // Process: the helper's context signals fromHost = fenceValue after all of its work
    int32_t width = 0, height = 0;
    wchar_t error[512]{};
};
static_assert(std::is_trivially_copyable_v<HostRequest> && std::is_trivially_copyable_v<HostReply>);

// Creating the helper, its first frame, and frames after a resize or a settings change create the model's features,
// which takes seconds; any other frame that takes longer than kHostFrameTimeoutMs means the helper is stuck.
constexpr DWORD kHostSlowTimeoutMs = 30000;
constexpr DWORD kHostFrameTimeoutMs = 2000;
constexpr DWORD kHostQuitTimeoutMs = 2000; // destructor: Quit, then this long for the helper to exit

void SetReplyError(HostReply& reply, const std::wstring& text) {
    reply.ok = 0;
    wcsncpy_s(reply.error, text.c_str(), _TRUNCATE);
}

std::wstring ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0 || n >= path.size()) return {};
    path.resize(n);
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

HRESULT CreateSharedTexture(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format,
                            ComPtr<ID3D11Texture2D>& texture, UINT extraBind = 0) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | extraBind;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    texture.Reset();
    return device->CreateTexture2D(&td, nullptr, &texture);
}

// Security descriptor that grants the pipe to the current user only (SDDL "D:P(A;;GA;;;<user SID>)").
class UserOnlySecurity {
public:
    bool Init() {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
        Handle token(raw);
        DWORD bytes = 0;
        GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &bytes);
        if (bytes == 0) return false;
        std::vector<BYTE> buffer(bytes);
        if (!GetTokenInformation(token.Get(), TokenUser, buffer.data(), bytes, &bytes)) return false;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) return false;
        const std::wstring sddl = std::format(L"D:P(A;;GA;;;{})", sid);
        LocalFree(sid);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
            return false;
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = descriptor_;
        attributes_.bInheritHandle = FALSE;
        return true;
    }
    ~UserOnlySecurity() {
        if (descriptor_) LocalFree(descriptor_);
    }
    SECURITY_ATTRIBUTES* Attributes() { return &attributes_; }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

// Per frame v, on the NR GPU's device of THIS process (remote_, which may be main_ itself):
//   [other GPU: up_ / guideUp_ Transfer] -> copy into the shared input / guide -> Signal(toHost, v) -> Flush -> request v
//   helper: Wait(toHost, v) -> bridge -> copy into the shared output -> Signal(fromHost, v) -> Flush -> reply
//   Wait(fromHost, v) -> [other GPU: down_ Transfer of the shared output back to the main GPU; same GPU: the shared
//   output itself is returned].
// The helper signals fromHost = v for every Process request it received once it was created, failed or not, and
// says so in the reply; this side waits for it whenever the reply says so. The helper rewrites the shared output for
// v+1 only after Wait(toHost, v+1), which this device signals after the consumers of v (the down_ copy, or the
// engine's own commands on the same GPU) were submitted; this device rewrites the shared input for v+1 after
// Wait(fromHost, v), i.e. after the helper has read it. So one copy of each shared texture is enough.
// If the helper dies or stops answering after a reply, its Signal may never execute. This device's context would
// then wait forever, so the same fence is also opened on a D3D12 device here and signalled to UINT64_MAX from the
// CPU, which releases every GPU waiter: when the helper's process exits (a registered wait), when a pipe operation
// fails or times out, and in the destructor.
class HostedNeuralBridge final : public NeuralBridge {
public:
    HostedNeuralBridge(GpuDevice& main, GpuDevice& remote) : main_(main), remote_(remote), sameGpu_(&main == &remote) {}

    ~HostedNeuralBridge() override {
        if (pipe_ && process_ && !broken_) {
            HostRequest quit;
            quit.op = HostOp::Quit;
            OVERLAPPED ov{};
            ov.hEvent = event_.Get();
            DWORD written = 0;
            ResetEvent(event_.Get());
            if (WriteFile(pipe_.Get(), &quit, sizeof(quit), &written, &ov) || GetLastError() == ERROR_IO_PENDING) {
                std::wstring ignored;
                Await(ov, written, ignored, kHostQuitTimeoutMs);
            }
        }
        // The helper exits on Quit or when the pipe breaks; the job object would also kill it with this process.
        if (process_ && WaitForSingleObject(process_.Get(), kHostQuitTimeoutMs) != WAIT_OBJECT_0) {
            MSR_LOG_WARN(L"Neural rendering helper did not exit; terminating it");
            TerminateProcess(process_.Get(), 1);
        }
        ReleaseGpuWaiters();
        if (exitWait_) UnregisterWaitEx(exitWait_, INVALID_HANDLE_VALUE); // waits for a running callback
    }

    bool Init(const std::wstring& modelPath, const NeuralSettings& settings, std::wstring& error) {
        if (modelPath.size() >= std::size(HostRequest{}.modelPath)) return Fail(error, L"the model path is too long");
        ComPtr<ID3D11Device5> device5;
        HRESULT hr = remote_.Device()->QueryInterface(IID_PPV_ARGS(&device5));
        if (SUCCEEDED(hr)) hr = remote_.Context()->QueryInterface(IID_PPV_ARGS(&context4_));
        if (SUCCEEDED(hr)) hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&toHost_));
        if (SUCCEEDED(hr)) hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fromHost_));
        if (FAILED(hr)) return Fail(error, L"shared D3D11 fences are unavailable: " + HrToString(hr));
        // The CPU-side release of fromHost (see above). D3D12 devices are per-adapter singletons in a process, so
        // this is usually the device CrossAdapterLink already uses.
        hr = D3D12CreateDevice(remote_.Adapter(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12_));
        HANDLE shared = nullptr;
        if (SUCCEEDED(hr)) hr = fromHost_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared);
        if (SUCCEEDED(hr)) {
            Handle owned(shared);
            hr = device12_->OpenSharedHandle(owned.Get(), IID_PPV_ARGS(&fromHost12_));
        }
        if (FAILED(hr)) return Fail(error, L"open the result fence on D3D12: " + HrToString(hr));
        if (!sameGpu_) {
            up_ = CrossAdapterLink::Create(main_, remote_, error);
            if (!up_) return false;
            guideUp_ = CrossAdapterLink::Create(main_, remote_, error);
            if (!guideUp_) return false;
            down_ = CrossAdapterLink::Create(remote_, main_, error);
            if (!down_) return false;
        }
        if (!Spawn(error)) return false;

        HostRequest create;
        create.op = HostOp::Create;
        create.settings = settings;
        create.adapter = remote_.Info().luid;
        wcsncpy_s(create.modelPath, modelPath.c_str(), _TRUNCATE);
        if (!ShareFence(toHost_.Get(), create.toHost, error) || !ShareFence(fromHost_.Get(), create.fromHost, error))
            return MarkBroken();
        HostReply reply;
        if (!Transact(create, reply, error, kHostSlowTimeoutMs)) return false;
        if (!reply.ok) return Fail(error, reply.error);
        live_ = reply.liveSettings != 0;
        if (sameGpu_)
            MSR_LOG_INFO(L"Neural rendering runs on '{}' (the main GPU) in a helper process", remote_.Info().name);
        else
            MSR_LOG_INFO(L"Neural rendering runs on '{}' in a helper process (main GPU '{}'); frames cross by {}",
                         remote_.Info().name, main_.Info().name, up_->Method());
        return true;
    }

    bool SupportsLiveSettings() const override { return live_; }

    bool UpdateSettings(const NeuralSettings& settings, std::wstring& error) override {
        HostRequest update;
        update.op = HostOp::Update;
        update.settings = settings;
        HostReply reply;
        if (!Transact(update, reply, error, kHostFrameTimeoutMs)) return false;
        if (!reply.ok) error = reply.error;
        slowNext_ = true; // the next frame may rebuild the model's features
        return reply.ok != 0;
    }

    bool Process(ID3D11Texture2D* texture, const RectI& rect, int64_t captureQpc, uint64_t sequence, bool reset,
                 const PictureSettings& picture, ID3D11Texture2D*& enhanced, RectI& enhancedRect,
                 std::wstring& error) override {
        return ProcessWithGuide(texture, rect, nullptr, captureQpc, sequence, reset, picture, enhanced, enhancedRect, error);
    }

    bool ProcessWithGuide(ID3D11Texture2D* texture, const RectI& rect, ID3D11Texture2D* flowGuide, int64_t captureQpc,
                          uint64_t sequence, bool reset, const PictureSettings& picture, ID3D11Texture2D*& enhanced,
                          RectI& enhancedRect, std::wstring& error) override {
        enhanced = nullptr;
        enhancedRect = {};
        if (broken_) return Fail(error, L"the helper process is no longer available");
        const Size size{static_cast<uint32_t>(rect.Width()), static_cast<uint32_t>(rect.Height())};
        if (!texture || size.Empty()) return Fail(error, L"empty frame");
        D3D11_TEXTURE2D_DESC sd{};
        texture->GetDesc(&sd);
        if (sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return Fail(error, L"the frame is not BGRA8");
        // On another GPU the frame first crosses to it (at 0,0); on the same GPU it is copied straight from `rect`.
        ID3D11Texture2D* remoteInput = sameGpu_ ? texture : up_->Transfer(texture, rect, error);
        if (!remoteInput) return false;
        const D3D11_BOX box = sameGpu_ ? D3D11_BOX{static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0,
                                                   static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1}
                                       : D3D11_BOX{0, 0, 0, size.width, size.height, 1};
        HostRequest request;
        request.op = HostOp::Process;
        if (!input_ || inputSize_ != size) {
            HRESULT hr = CreateSharedTexture(remote_.Device(), size.width, size.height, DXGI_FORMAT_B8G8R8A8_UNORM, input_);
            if (SUCCEEDED(hr))
                hr = CreateSharedTexture(remote_.Device(), size.width, size.height, DXGI_FORMAT_B8G8R8A8_UNORM, output_,
                                         D3D11_BIND_RENDER_TARGET); // returned to the engine on the same GPU
            if (FAILED(hr)) {
                input_.Reset();
                return Fail(error, std::format(L"shared {}x{} frame: {}", size.width, size.height, HrToString(hr)));
            }
            inputSize_ = size;
            texturesChanged_ = true;
        }
        remote_.Context()->CopySubresourceRegion(input_.Get(), 0, 0, 0, 0, remoteInput, 0, &box);
        if (flowGuide) {
            D3D11_TEXTURE2D_DESC gd{};
            flowGuide->GetDesc(&gd);
            ID3D11Texture2D* remoteGuide = sameGpu_ ? flowGuide : guideUp_->Transfer(
                flowGuide, RectI{0, 0, static_cast<int32_t>(gd.Width), static_cast<int32_t>(gd.Height)}, error);
            if (!remoteGuide) return false;
            if (!guide_ || guideDesc_.Width != gd.Width || guideDesc_.Height != gd.Height || guideDesc_.Format != gd.Format) {
                const HRESULT hr = CreateSharedTexture(remote_.Device(), gd.Width, gd.Height, gd.Format, guide_);
                if (FAILED(hr))
                    return Fail(error, std::format(L"shared {}x{} flow guide: {}", gd.Width, gd.Height, HrToString(hr)));
                guide_->GetDesc(&guideDesc_);
                texturesChanged_ = true;
            }
            D3D11_TEXTURE2D_DESC rd{};
            remoteGuide->GetDesc(&rd);
            if (rd.Format != guideDesc_.Format || rd.Width < gd.Width || rd.Height < gd.Height)
                return Fail(error, L"the flow guide does not match its shared copy");
            const D3D11_BOX guideBox{0, 0, 0, gd.Width, gd.Height, 1};
            remote_.Context()->CopySubresourceRegion(guide_.Get(), 0, 0, 0, 0, remoteGuide, 0, &guideBox);
            request.hasGuide = 1;
        }
        if (texturesChanged_) {
            // A handle that was already duplicated into the helper cannot be taken back, so a failure here ends
            // this bridge; the engine reports the error and neural rendering has to be started again.
            if (!ShareTexture(input_.Get(), request.input, error) || !ShareTexture(output_.Get(), request.output, error) ||
                (guide_ && !ShareTexture(guide_.Get(), request.guide, error)))
                return MarkBroken();
            request.newTextures = 1;
        }
        HRESULT hr = context4_->Signal(toHost_.Get(), ++fenceValue_);
        if (FAILED(hr)) return Fail(error, L"signal the helper's fence: " + HrToString(hr));
        remote_.Context()->Flush();
        request.width = size.width;
        request.height = size.height;
        request.reset = reset ? 1u : 0u;
        request.fenceValue = fenceValue_;
        request.sequence = sequence;
        request.captureQpc = captureQpc;
        request.picture = picture;
        HostReply reply;
        const DWORD timeout = slowNext_ || request.newTextures ? kHostSlowTimeoutMs : kHostFrameTimeoutMs;
        if (!Transact(request, reply, error, timeout)) return false;
        if (reply.signalled) {
            hr = context4_->Wait(fromHost_.Get(), fenceValue_);
            if (FAILED(hr)) {
                Fail(error, L"wait for the helper's fence: " + HrToString(hr));
                return MarkBroken();
            }
        }
        if (!reply.ok) {
            // Textures the helper could not open are sent again with the next frame (texturesChanged_ stays set).
            error = reply.error;
            return false;
        }
        if (!reply.signalled) {
            Fail(error, L"the helper did not order its result");
            return MarkBroken();
        }
        texturesChanged_ = false;
        slowNext_ = false;
        if (reply.width <= 0 || reply.height <= 0 || static_cast<uint32_t>(reply.width) > size.width ||
            static_cast<uint32_t>(reply.height) > size.height)
            return Fail(error, L"the helper returned an unexpected frame size");
        const RectI result{0, 0, reply.width, reply.height};
        ID3D11Texture2D* back = sameGpu_ ? output_.Get() : down_->Transfer(output_.Get(), result, error);
        if (!back) return false;
        enhanced = back;
        enhancedRect = result;
        return true;
    }

private:
    static bool Fail(std::wstring& error, const std::wstring& what) {
        error = L"Neural rendering helper: " + what;
        return false;
    }

    // From now on nothing is sent; GPU work waiting for the helper is released.
    bool MarkBroken() {
        broken_ = true;
        ReleaseGpuWaiters();
        return false;
    }

    void ReleaseGpuWaiters() {
        if (fromHost12_) fromHost12_->Signal(UINT64_MAX);
    }

    static void CALLBACK OnHelperExit(void* fence, BOOLEAN) {
        static_cast<ID3D12Fence*>(fence)->Signal(UINT64_MAX); // ID3D12Fence is free-threaded
    }

    bool Spawn(std::wstring& error) {
        const std::wstring directory = ExecutableDirectory();
        const std::wstring exe = directory + L"\\MoonSR_NrHost.exe";
        if (directory.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
            return Fail(error, L"MoonSR_NrHost.exe is missing beside the executable");
        UserOnlySecurity security;
        if (!security.Init()) return Fail(error, std::format(L"pipe security descriptor failed ({})", GetLastError()));
        static std::atomic<uint32_t> counter{0};
        const std::wstring pipeName = std::format(L"\\\\.\\pipe\\MoonSR-NrHost-{}-{}", GetCurrentProcessId(), ++counter);
        pipe_.Reset(CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                     sizeof(HostRequest), sizeof(HostReply), 0, security.Attributes()));
        if (!pipe_) return Fail(error, std::format(L"CreateNamedPipe failed ({})", GetLastError()));
        event_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        job_.Reset(CreateJobObjectW(nullptr, nullptr));
        if (!event_ || !job_) return Fail(error, std::format(L"CreateEvent / CreateJobObject failed ({})", GetLastError()));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return Fail(error, std::format(L"SetInformationJobObject failed ({})", GetLastError()));

        std::wstring commandLine = std::format(L"\"{}\" --pipe {}", exe, pipeName);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                            nullptr, directory.c_str(), &si, &pi))
            return Fail(error, std::format(L"CreateProcess failed ({})", GetLastError()));
        process_.Reset(pi.hProcess);
        Handle thread(pi.hThread);
        if (!AssignProcessToJobObject(job_.Get(), process_.Get()))
            MSR_LOG_WARN(L"Neural rendering helper: AssignProcessToJobObject failed ({}); a crash of this process may "
                         L"leave it running until its pipe breaks", GetLastError());
        if (!RegisterWaitForSingleObject(&exitWait_, process_.Get(), OnHelperExit, fromHost12_.Get(), INFINITE,
                                         WT_EXECUTEONLYONCE)) {
            exitWait_ = nullptr;
            TerminateProcess(process_.Get(), 1);
            return Fail(error, std::format(L"RegisterWaitForSingleObject failed ({})", GetLastError()));
        }
        if (ResumeThread(thread.Get()) == static_cast<DWORD>(-1)) {
            TerminateProcess(process_.Get(), 1);
            return Fail(error, std::format(L"ResumeThread failed ({})", GetLastError()));
        }

        OVERLAPPED ov{};
        ov.hEvent = event_.Get();
        ResetEvent(event_.Get());
        if (!ConnectNamedPipe(pipe_.Get(), &ov)) {
            const DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                DWORD unused = 0;
                if (!Await(ov, unused, error, kHostSlowTimeoutMs)) return false;
            } else if (e != ERROR_PIPE_CONNECTED) {
                return Fail(error, std::format(L"ConnectNamedPipe failed ({})", e));
            }
        }
        ULONG client = 0;
        if (!GetNamedPipeClientProcessId(pipe_.Get(), &client) || client != GetProcessId(process_.Get())) {
            Fail(error, L"an unexpected process connected to the pipe");
            return MarkBroken();
        }
        return true;
    }

    // Waits for an overlapped pipe operation, the helper's exit or the timeout, whichever comes first. On failure the
    // helper is given up: it is terminated if still running, and GPU waits for it are released.
    bool Await(OVERLAPPED& ov, DWORD& bytes, std::wstring& error, DWORD timeoutMs) {
        const HANDLE waits[] = {ov.hEvent, process_.Get()};
        const DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
        if (r != WAIT_OBJECT_0) {
            const DWORD waitError = GetLastError();
            CancelIoEx(pipe_.Get(), &ov);
            GetOverlappedResult(pipe_.Get(), &ov, &bytes, TRUE);
            DWORD code = 0;
            if (r == WAIT_OBJECT_0 + 1 && GetExitCodeProcess(process_.Get(), &code)) {
                Fail(error, std::format(L"the helper process exited (code {})", code));
            } else {
                TerminateProcess(process_.Get(), 1);
                if (r == WAIT_TIMEOUT)
                    Fail(error, std::format(L"the helper process did not answer within {} ms", timeoutMs));
                else
                    Fail(error, std::format(L"pipe wait failed ({})", waitError));
            }
            return MarkBroken();
        }
        if (!GetOverlappedResult(pipe_.Get(), &ov, &bytes, FALSE)) {
            Fail(error, std::format(L"pipe I/O failed ({})", GetLastError()));
            return MarkBroken();
        }
        return true;
    }

    bool Transact(const HostRequest& request, HostReply& reply, std::wstring& error, DWORD timeoutMs) {
        if (broken_) return Fail(error, L"the helper process is no longer available");
        OVERLAPPED ov{};
        ov.hEvent = event_.Get();
        DWORD bytes = 0;
        ResetEvent(event_.Get());
        if (!WriteFile(pipe_.Get(), &request, sizeof(request), &bytes, &ov) && GetLastError() != ERROR_IO_PENDING) {
            Fail(error, std::format(L"pipe write failed ({})", GetLastError()));
            return MarkBroken();
        }
        if (!Await(ov, bytes, error, timeoutMs)) return false;
        ov = {};
        ov.hEvent = event_.Get();
        ResetEvent(event_.Get());
        if (!ReadFile(pipe_.Get(), &reply, sizeof(reply), &bytes, &ov) && GetLastError() != ERROR_IO_PENDING) {
            Fail(error, std::format(L"pipe read failed ({})", GetLastError()));
            return MarkBroken();
        }
        if (!Await(ov, bytes, error, timeoutMs)) return false;
        if (bytes != sizeof(reply)) {
            Fail(error, L"malformed reply (a different MoonSR_NrHost.exe version?)");
            return MarkBroken();
        }
        reply.error[std::size(reply.error) - 1] = L'\0';
        return true;
    }

    // Duplicates `own` into the helper; `own` is closed either way.
    bool Duplicate(HANDLE own, uint64_t& remoteValue, std::wstring& error) {
        Handle local(own);
        HANDLE target = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), local.Get(), process_.Get(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return Fail(error, std::format(L"DuplicateHandle failed ({})", GetLastError()));
        remoteValue = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(target));
        return true;
    }

    bool ShareFence(ID3D11Fence* fence, uint64_t& remoteValue, std::wstring& error) {
        HANDLE h = nullptr;
        const HRESULT hr = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
        if (FAILED(hr)) return Fail(error, L"share a fence: " + HrToString(hr));
        return Duplicate(h, remoteValue, error);
    }

    bool ShareTexture(ID3D11Texture2D* texture, uint64_t& remoteValue, std::wstring& error) {
        ComPtr<IDXGIResource1> resource;
        HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(&resource));
        HANDLE h = nullptr;
        if (SUCCEEDED(hr))
            hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
        if (FAILED(hr)) return Fail(error, L"share a texture: " + HrToString(hr));
        return Duplicate(h, remoteValue, error);
    }

    GpuDevice& main_;
    GpuDevice& remote_;
    const bool sameGpu_;
    Handle job_, process_, pipe_, event_; // declared first: destroyed after everything below
    HANDLE exitWait_ = nullptr;           // RegisterWaitForSingleObject; unregistered in the destructor
    ComPtr<ID3D12Device> device12_;
    ComPtr<ID3D12Fence> fromHost12_;
    std::unique_ptr<CrossAdapterLink> up_, guideUp_, down_;
    ComPtr<ID3D11DeviceContext4> context4_;
    ComPtr<ID3D11Fence> toHost_, fromHost_;
    uint64_t fenceValue_ = 0;
    ComPtr<ID3D11Texture2D> input_, output_, guide_;
    Size inputSize_;
    D3D11_TEXTURE2D_DESC guideDesc_{};
    bool texturesChanged_ = false;
    bool slowNext_ = true; // the first frame creates the model's features
    bool live_ = false;
    bool broken_ = false;
};

// In-process NVIDIA neural rendering whose first feature creation may be refused because another GPU holds the
// oldest device of this process (the engine guesses which GPU that is; see the header). On that refusal the stage
// moves to the helper process once, and the frame is processed again there.
class NeuralBridgeWithHostFallback final : public NeuralBridge {
public:
    NeuralBridgeWithHostFallback(GpuDevice& main, GpuDevice& nr, std::unique_ptr<NeuralBridge> inner,
                                 const std::wstring& modelPath, const NeuralSettings& settings)
        : main_(main), nr_(nr), inner_(std::move(inner)), modelPath_(modelPath), settings_(settings) {}

    bool SupportsLiveSettings() const override { return inner_ && inner_->SupportsLiveSettings(); }

    bool UpdateSettings(const NeuralSettings& settings, std::wstring& error) override {
        settings_ = settings;
        if (!inner_) return false;
        return inner_->UpdateSettings(settings, error);
    }

    bool Process(ID3D11Texture2D* texture, const RectI& rect, int64_t captureQpc, uint64_t sequence, bool reset,
                 const PictureSettings& picture, ID3D11Texture2D*& enhanced, RectI& enhancedRect,
                 std::wstring& error) override {
        return ProcessWithGuide(texture, rect, nullptr, captureQpc, sequence, reset, picture, enhanced, enhancedRect, error);
    }

    bool ProcessWithGuide(ID3D11Texture2D* texture, const RectI& rect, ID3D11Texture2D* flowGuide, int64_t captureQpc,
                          uint64_t sequence, bool reset, const PictureSettings& picture, ID3D11Texture2D*& enhanced,
                          RectI& enhancedRect, std::wstring& error) override {
        if (!inner_) {
            error = L"Neural rendering is unavailable.";
            return false;
        }
        if (inner_->ProcessWithGuide(texture, rect, flowGuide, captureQpc, sequence, reset, picture, enhanced,
                                     enhancedRect, error)) {
            proven_ = true;
            return true;
        }
        // NVSDK_NGX_Result_FAIL_PlatformError, as the bridge reports a refused feature creation.
        if (proven_ || hosted_ || error.find(L"create=0xBAD00002") == std::wstring::npos) return false;
        hosted_ = true;
        MSR_LOG_WARN(L"Neural rendering: NVIDIA's runtime refused '{}' in this process (NGX FAIL_PlatformError: another "
                     L"GPU holds this process's oldest device); moving the stage to a helper process", nr_.Info().name);
        inner_.reset();
        std::wstring hostError;
        inner_ = CreateHostedNeuralBridge(main_, nr_, modelPath_, settings_, hostError);
        if (!inner_) {
            error = hostError;
            return false;
        }
        reset = true; // the hosted model starts without history
        return inner_->ProcessWithGuide(texture, rect, flowGuide, captureQpc, sequence, reset, picture, enhanced,
                                        enhancedRect, error);
    }

private:
    GpuDevice& main_;
    GpuDevice& nr_;
    std::unique_ptr<NeuralBridge> inner_;
    std::wstring modelPath_;
    NeuralSettings settings_;
    bool proven_ = false;
    bool hosted_ = false;
};

// The helper side (MoonSR_NrHost.exe, its main thread). Every handle value in a request was duplicated into this
// process and is owned here.
class NeuralHost {
public:
    explicit NeuralHost(HANDLE pipe) : pipe_(pipe) {}

    ~NeuralHost() {
        bridge_.reset(); // before the device it runs on
        guide_.Reset();
        output_.Reset();
        input_.Reset();
    }

    int Run() {
        for (;;) {
            HostRequest request;
            DWORD bytes = 0;
            if (!ReadFile(pipe_, &request, sizeof(request), &bytes, nullptr) || bytes != sizeof(request)) return 0;
            if (request.protocol != kHostProtocol) return 2;
            if (request.op == HostOp::Quit) return 0;
            HostReply reply;
            switch (request.op) {
            case HostOp::Create: Create(request, reply); break;
            case HostOp::Update: Update(request, reply); break;
            case HostOp::Process: Process(request, reply); break;
            default: SetReplyError(reply, L"unknown request"); break;
            }
            if (!WriteFile(pipe_, &reply, sizeof(reply), &bytes, nullptr)) return 0;
        }
    }

private:
    static HANDLE Owned(uint64_t value) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)); }

    void Create(const HostRequest& request, HostReply& reply) {
        Handle toHost(Owned(request.toHost)), fromHost(Owned(request.fromHost));
        if (device_) return SetReplyError(reply, L"already created");
        std::wstring error;
        // The first device of this process, so NGX accepts features on its adapter (see the header).
        device_ = GpuDevice::Create(&request.adapter, error);
        if (!device_) return SetReplyError(reply, error);
        RaiseGpuPriority(device_->Device()); // the bridge's D3D12 queue follows the device's priority
        ComPtr<ID3D11Device5> device5;
        HRESULT hr = device_->Device()->QueryInterface(IID_PPV_ARGS(&device5));
        if (SUCCEEDED(hr)) hr = device_->Context()->QueryInterface(IID_PPV_ARGS(&context4_));
        if (SUCCEEDED(hr)) hr = device5->OpenSharedFence(toHost.Get(), IID_PPV_ARGS(&toHost_));
        if (SUCCEEDED(hr)) hr = device5->OpenSharedFence(fromHost.Get(), IID_PPV_ARGS(&fromHost_));
        if (FAILED(hr)) {
            fromHost_.Reset(); // Process must not claim it signals a fence it does not have
            return SetReplyError(reply, L"open the shared fences: " + HrToString(hr));
        }
        const std::wstring modelPath(request.modelPath, wcsnlen(request.modelPath, std::size(request.modelPath)));
        bridge_ = NeuralBridge::Create(*device_, modelPath, request.settings, error);
        if (!bridge_) return SetReplyError(reply, error);
        reply.ok = 1;
        reply.liveSettings = bridge_->SupportsLiveSettings() ? 1u : 0u;
    }

    void Update(const HostRequest& request, HostReply& reply) {
        if (!bridge_) return SetReplyError(reply, L"not created");
        std::wstring error;
        if (!bridge_->UpdateSettings(request.settings, error)) return SetReplyError(reply, error);
        reply.ok = 1;
    }

    bool OpenTexture(uint64_t value, ComPtr<ID3D11Texture2D>& texture, std::wstring& error) {
        Handle h(Owned(value));
        texture.Reset();
        if (!h) return true;
        const HRESULT hr = device_->Device()->OpenSharedResource1(h.Get(), IID_PPV_ARGS(&texture));
        if (FAILED(hr)) error = L"open a shared texture: " + HrToString(hr);
        return SUCCEEDED(hr);
    }

    // Once created, every Process request ends with Signal(fromHost, fenceValue) on this context, after whatever
    // was submitted for it, failed or not, so the other side never waits for a value that does not come.
    void Process(const HostRequest& request, HostReply& reply) {
        if (!device_ || !context4_ || !fromHost_) {
            for (uint64_t h : {request.input, request.output, request.guide}) Handle owned(Owned(h));
            return SetReplyError(reply, L"not created");
        }
        ProcessFrame(request, reply);
        const HRESULT hr = context4_->Signal(fromHost_.Get(), request.fenceValue);
        device_->Context()->Flush();
        if (SUCCEEDED(hr))
            reply.signalled = 1;
        else if (reply.ok)
            SetReplyError(reply, L"signal the result: " + HrToString(hr));
    }

    void ProcessFrame(const HostRequest& request, HostReply& reply) {
        std::wstring error;
        if (request.newTextures) {
            // Open all three (each call closes its handle) before reporting a failure.
            const bool in = OpenTexture(request.input, input_, error);
            const bool out = OpenTexture(request.output, output_, error);
            const bool guide = OpenTexture(request.guide, guide_, error);
            if (!in || !out || !guide) return SetReplyError(reply, error);
        }
        if (!bridge_ || !input_ || !output_) return SetReplyError(reply, L"not ready");
        if (request.hasGuide && !guide_) return SetReplyError(reply, L"the flow guide is missing");
        const HRESULT hr = context4_->Wait(toHost_.Get(), request.fenceValue);
        if (FAILED(hr)) return SetReplyError(reply, L"wait for the frame: " + HrToString(hr));
        ID3D11Texture2D* enhanced = nullptr;
        RectI rect;
        const RectI full{0, 0, static_cast<int32_t>(request.width), static_cast<int32_t>(request.height)};
        if (!bridge_->ProcessWithGuide(input_.Get(), full, request.hasGuide ? guide_.Get() : nullptr, request.captureQpc,
                                       request.sequence, request.reset != 0, request.picture, enhanced, rect, error))
            return SetReplyError(reply, error);
        D3D11_TEXTURE2D_DESC od{}, ed{};
        output_->GetDesc(&od);
        enhanced->GetDesc(&ed);
        if (ed.Format != od.Format) return SetReplyError(reply, L"the bridge returned an unexpected format");
        if (rect.Empty() || rect.left < 0 || rect.top < 0 || static_cast<uint32_t>(rect.right) > ed.Width ||
            static_cast<uint32_t>(rect.bottom) > ed.Height || static_cast<uint32_t>(rect.Width()) > od.Width ||
            static_cast<uint32_t>(rect.Height()) > od.Height)
            return SetReplyError(reply, L"the bridge returned an unexpected frame size");
        const D3D11_BOX box{static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0, static_cast<UINT>(rect.right),
                            static_cast<UINT>(rect.bottom), 1};
        device_->Context()->CopySubresourceRegion(output_.Get(), 0, 0, 0, 0, enhanced, 0, &box);
        reply.ok = 1;
        reply.width = rect.Width();
        reply.height = rect.Height();
    }

    HANDLE pipe_; // borrowed from RunNeuralHost
    std::unique_ptr<GpuDevice> device_;
    std::unique_ptr<NeuralBridge> bridge_;
    ComPtr<ID3D11DeviceContext4> context4_;
    ComPtr<ID3D11Fence> toHost_, fromHost_;
    ComPtr<ID3D11Texture2D> input_, output_, guide_;
};
} // namespace

std::unique_ptr<FrameGenerator> CreateRemoteFrameGenerator(GpuDevice& main, GpuDevice& remote, std::wstring& error) {
    auto g = std::make_unique<RemoteFrameGenerator>(main, remote);
    if (!g->Init(error)) return nullptr;
    return g;
}

std::unique_ptr<NeuralBridge> CreateRemoteNeuralBridge(GpuDevice& main, GpuDevice& remote,
                                                      std::unique_ptr<NeuralBridge> inner, std::wstring& error) {
    if (!inner) {
        error = L"Neural rendering bridge was not created.";
        return nullptr;
    }
    auto b = std::make_unique<RemoteNeuralBridge>(main, remote, std::move(inner));
    if (!b->Init(error)) return nullptr;
    return b;
}

std::unique_ptr<NeuralBridge> CreateHostedNeuralBridge(GpuDevice& main, GpuDevice& remote, const std::wstring& modelPath,
                                                      const NeuralSettings& settings, std::wstring& error) {
    auto b = std::make_unique<HostedNeuralBridge>(main, remote);
    if (!b->Init(modelPath, settings, error)) return nullptr;
    return b;
}

std::unique_ptr<NeuralBridge> WithHostedFallback(GpuDevice& main, GpuDevice& nr, std::unique_ptr<NeuralBridge> inProcess,
                                                const std::wstring& modelPath, const NeuralSettings& settings) {
    if (!inProcess) return nullptr;
    return std::make_unique<NeuralBridgeWithHostFallback>(main, nr, std::move(inProcess), modelPath, settings);
}

int RunNeuralHost(int argc, wchar_t** argv) {
    if (argc != 3 || std::wstring_view(argv[1]) != L"--pipe") return 2;
    if (!std::wstring_view(argv[2]).starts_with(L"\\\\.\\pipe\\MoonSR-NrHost-")) return 2;
    std::wstring local(32768, L'\0');
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (n > 0 && n < local.size()) {
        const std::wstring directory = local.substr(0, n) + L"\\MoonSR";
        CreateDirectoryW(directory.c_str(), nullptr);
        InitLog(std::format(L"{}\\moonsr_nrhost_{}.log", directory, GetCurrentProcessId()));
    }
    // Identification only: the server may learn who connected but cannot act as this process.
    Handle pipe(CreateFileW(argv[2], GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (!pipe) {
        MSR_LOG_ERROR(L"Neural rendering helper: cannot open the pipe ({})", GetLastError());
        return 3;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.Get(), &mode, nullptr, nullptr)) {
        MSR_LOG_ERROR(L"Neural rendering helper: SetNamedPipeHandleState failed ({})", GetLastError());
        return 3;
    }
    NeuralHost host(pipe.Get());
    return host.Run();
}

} // namespace moonsr
