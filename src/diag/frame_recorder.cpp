// FrameRecorder implementation.
//
// Threading: Begin / Capture / Active / PollFinished are called by the engine pipeline thread (the owner of the
// immediate context); all D3D work happens there. A single internal worker thread encodes PNGs with WIC and writes
// index.csv / info.txt. Hand-off is a mutex + condition variable guarded bounded queue. The worker never touches D3D.
#include "diag/frame_recorder.h"

#include "core/log.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace moonsr {
namespace {

constexpr size_t kMaxQueued = 64;

struct Job {
    std::vector<uint8_t> pixels;  // BGRA, tightly packed, alpha forced to 255
    uint32_t width = 0, height = 0;
    std::wstring directory;
    std::wstring fileName;
    std::string csvRow;           // already formatted, without newline
    bool finalize = false;        // last job of a recording: write index.csv + info.txt afterwards
    // Used only when finalize is set.
    std::string description;
    std::string startTime;
    uint32_t inputCount = 0, outputCount = 0;
    int64_t qpcFreq = 0;
};

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool IsSupportedFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

class FrameRecorderImpl final : public FrameRecorder {
public:
    explicit FrameRecorderImpl(GpuDevice& device) : device_(device) {
        worker_ = std::thread([this] { WorkerMain(); });
    }

    ~FrameRecorderImpl() override {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cvWork_.notify_all();
        cvSpace_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    bool Begin(const std::wstring& directory, uint32_t outputFrames, const std::wstring& description) override {
        if (active_.load()) {
            MSR_LOG_WARN(L"FrameRecorder::Begin ignored: a recording is already active");
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(directory), ec);
        if (ec || !std::filesystem::is_directory(std::filesystem::path(directory), ec)) {
            MSR_LOG_ERROR(L"FrameRecorder: cannot create directory '{}'", directory);
            return false;
        }
        directory_ = directory;
        description_ = Narrow(description);
        targetOutputs_ = std::clamp<uint32_t>(outputFrames, 1u, 240u);
        outputsCaptured_ = 0;
        inputsCaptured_ = 0;
        LARGE_INTEGER q{}, fq{};
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&fq);
        startQpc_ = q.QuadPart;
        qpcFreq_ = fq.QuadPart;
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char tb[64];
        snprintf(tb, sizeof(tb), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        startTime_ = tb;
        active_.store(true);
        MSR_LOG_INFO(L"FrameRecorder: recording {} output frames into '{}'", targetOutputs_, directory_);
        return true;
    }

    bool Active() const override { return active_.load(); }

    void Capture(ID3D11Texture2D* texture, const RectI& region, const RecordedFrameInfo& info) override {
        if (!active_.load() || !texture) return;
        try {
            CaptureImpl(texture, region, info);
        } catch (const std::exception& e) {
            MSR_LOG_ERROR(L"FrameRecorder::Capture exception: {}", Widen(e.what()));
        } catch (...) {
            MSR_LOG_ERROR(L"FrameRecorder::Capture unknown exception");
        }
    }

    bool PollFinished(std::wstring& directory) override {
        std::lock_guard lock(mutex_);
        if (finished_.empty()) return false;
        directory = std::move(finished_.front());
        finished_.pop_front();
        return true;
    }

private:
    static std::wstring Widen(const char* s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (n <= 1) return {};
        std::wstring w(static_cast<size_t>(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
        return w;
    }

    bool EnsureStaging(DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
        if (staging_ && stagingW_ == w && stagingH_ == h && stagingFmt_ == fmt) return true;
        staging_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT hr = device_.Device()->CreateTexture2D(&td, nullptr, &staging_);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"FrameRecorder: creating {}x{} staging texture failed: {}", w, h, HrToString(hr));
            return false;
        }
        stagingW_ = w;
        stagingH_ = h;
        stagingFmt_ = fmt;
        return true;
    }

    void CaptureImpl(ID3D11Texture2D* texture, const RectI& regionIn, const RecordedFrameInfo& info) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (!IsSupportedFormat(desc.Format) || desc.SampleDesc.Count != 1 || desc.ArraySize != 1) {
            if (!warnedFormat_) {
                warnedFormat_ = true;
                MSR_LOG_WARN(L"FrameRecorder: unsupported texture (format {}, samples {}); frame ignored",
                             static_cast<uint32_t>(desc.Format), desc.SampleDesc.Count);
            }
            return;
        }
        if (info.kind == RecordedKind::Input) {
            // Inputs after the last output are ignored by Active() flipping; nothing extra to do here.
        }

        // Clamp the region to the texture; empty region = whole texture.
        RectI r = regionIn;
        if (r.Empty()) r = RectI{0, 0, static_cast<int32_t>(desc.Width), static_cast<int32_t>(desc.Height)};
        r.left = std::clamp<int32_t>(r.left, 0, static_cast<int32_t>(desc.Width));
        r.right = std::clamp<int32_t>(r.right, 0, static_cast<int32_t>(desc.Width));
        r.top = std::clamp<int32_t>(r.top, 0, static_cast<int32_t>(desc.Height));
        r.bottom = std::clamp<int32_t>(r.bottom, 0, static_cast<int32_t>(desc.Height));
        if (r.Empty()) {
            MSR_LOG_WARN(L"FrameRecorder: region does not intersect the texture; frame ignored");
            return;
        }
        const uint32_t w = static_cast<uint32_t>(r.Width());
        const uint32_t h = static_cast<uint32_t>(r.Height());

        if (!EnsureStaging(desc.Format, w, h)) return;
        ID3D11DeviceContext* ctx = device_.Context();
        const D3D11_BOX box{static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0, static_cast<UINT>(r.right),
                            static_cast<UINT>(r.bottom), 1};
        ctx->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, texture, 0, &box);
        D3D11_MAPPED_SUBRESOURCE map{};
        const HRESULT hr = ctx->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"FrameRecorder: Map staging failed: {}", HrToString(hr));
            return;
        }
        Job job;
        job.width = w;
        job.height = h;
        job.pixels.resize(static_cast<size_t>(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* src = static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch;
            uint8_t* dst = job.pixels.data() + static_cast<size_t>(y) * w * 4;
            memcpy(dst, src, static_cast<size_t>(w) * 4);
            for (uint32_t x = 0; x < w; ++x) dst[x * 4 + 3] = 255;
        }
        ctx->Unmap(staging_.Get(), 0);

        // Naming and bookkeeping.
        wchar_t name[96];
        const char* kindStr = "real";
        if (info.kind == RecordedKind::Input) {
            kindStr = "input";
            swprintf_s(name, L"in_%04llu.png", static_cast<unsigned long long>(info.realIndex));
            ++inputsCaptured_;
        } else if (info.kind == RecordedKind::Generated) {
            kindStr = "generated";
            swprintf_s(name, L"out_%04u_gen_t%.2f.png", outputsCaptured_ + 1, static_cast<double>(info.phase));
            ++outputsCaptured_;
        } else {
            swprintf_s(name, L"out_%04u_real.png", outputsCaptured_ + 1);
            ++outputsCaptured_;
        }
        job.directory = directory_;
        job.fileName = name;
        const int64_t actual = info.actualQpc != 0 ? info.actualQpc : NowQpc();
        const double ms = static_cast<double>(actual - startQpc_) * 1000.0 / static_cast<double>(qpcFreq_);
        char row[512];
        snprintf(row, sizeof(row), "%s,%s,%.4f,%llu,%lld,%lld,%.3f,%u,%u", Narrow(job.fileName).c_str(), kindStr,
                 static_cast<double>(info.phase), static_cast<unsigned long long>(info.realIndex),
                 static_cast<long long>(info.dueQpc), static_cast<long long>(info.actualQpc), ms, w, h);
        job.csvRow = row;

        if (info.kind != RecordedKind::Input && outputsCaptured_ >= targetOutputs_) {
            job.finalize = true;
            job.description = description_;
            job.startTime = startTime_;
            job.inputCount = inputsCaptured_;
            job.outputCount = outputsCaptured_;
            job.qpcFreq = qpcFreq_;
            active_.store(false);
        }

        std::unique_lock lock(mutex_);
        cvSpace_.wait(lock, [&] { return queue_.size() < kMaxQueued || stop_; });
        queue_.push_back(std::move(job));
        lock.unlock();
        cvWork_.notify_one();
    }

    static int64_t NowQpc() {
        LARGE_INTEGER q{};
        QueryPerformanceCounter(&q);
        return q.QuadPart;
    }

    // ---- worker thread ----

    void WorkerMain() {
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IWICImagingFactory> factory;
        if (SUCCEEDED(coHr) || coHr == RPC_E_CHANGED_MODE) {
            const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            if (FAILED(hr)) MSR_LOG_ERROR(L"FrameRecorder: WIC factory creation failed: {}", HrToString(hr));
        } else {
            MSR_LOG_ERROR(L"FrameRecorder: CoInitializeEx failed: {}", HrToString(coHr));
        }

        std::vector<std::string> rows;  // rows of the recording currently being written
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                cvWork_.wait(lock, [&] { return !queue_.empty() || stop_; });
                if (queue_.empty()) break;  // stop requested and drained
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            cvSpace_.notify_one();

            const std::filesystem::path dir(job.directory);
            std::wstring err;
            if (WritePng(factory.Get(), dir / job.fileName, job, err)) {
                rows.push_back(std::move(job.csvRow));
            } else {
                MSR_LOG_ERROR(L"FrameRecorder: writing '{}' failed: {}", job.fileName, err);
            }
            if (job.finalize) {
                std::string csv = "file,kind,phase,realIndex,dueQpc,actualQpc,msSinceStart,width,height\n";
                for (const auto& r : rows) csv += r + "\n";
                if (!WriteTextFile(dir / L"index.csv", csv)) MSR_LOG_ERROR(L"FrameRecorder: writing index.csv failed");
                char buf[256];
                std::string info = "MoonSR frame recording\n";
                info += "description: " + job.description + "\n";
                info += "start: " + job.startTime + "\n";
                snprintf(buf, sizeof(buf), "input_frames: %u\noutput_frames: %u\nqpc_frequency: %lld\n", job.inputCount,
                         job.outputCount, static_cast<long long>(job.qpcFreq));
                info += buf;
                if (!WriteTextFile(dir / L"info.txt", info)) MSR_LOG_ERROR(L"FrameRecorder: writing info.txt failed");
                rows.clear();
                std::lock_guard lock(mutex_);
                finished_.push_back(job.directory);
            }
        }
        factory.Reset();
        if (SUCCEEDED(coHr)) CoUninitialize();
    }

    static bool WritePng(IWICImagingFactory* f, const std::filesystem::path& path, const Job& job, std::wstring& err) {
        if (!f) { err = L"WIC factory unavailable"; return false; }
        ComPtr<IWICStream> stream;
        HRESULT hr = f->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        ComPtr<IWICBitmapEncoder> enc;
        if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
        if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        ComPtr<IWICBitmapFrameEncode> fr;
        if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, nullptr);
        if (SUCCEEDED(hr)) hr = fr->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = fr->SetSize(job.width, job.height);
        // 24bpp BGR: opaque by construction, so viewers never show transparency.
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&fmt);
        if (FAILED(hr)) { err = L"encoder setup " + HrToString(hr); return false; }
        std::vector<uint8_t> rgb(static_cast<size_t>(job.width) * job.height * 3);
        for (size_t i = 0, n = static_cast<size_t>(job.width) * job.height; i < n; ++i) {
            rgb[i * 3 + 0] = job.pixels[i * 4 + 0];
            rgb[i * 3 + 1] = job.pixels[i * 4 + 1];
            rgb[i * 3 + 2] = job.pixels[i * 4 + 2];
        }
        hr = fr->WritePixels(job.height, job.width * 3, static_cast<UINT>(rgb.size()), rgb.data());
        if (SUCCEEDED(hr)) hr = fr->Commit();
        if (SUCCEEDED(hr)) hr = enc->Commit();
        if (FAILED(hr)) { err = L"PNG write " + HrToString(hr); return false; }
        return true;
    }

    GpuDevice& device_;

    // Engine-thread state.
    ComPtr<ID3D11Texture2D> staging_;
    uint32_t stagingW_ = 0, stagingH_ = 0;
    DXGI_FORMAT stagingFmt_ = DXGI_FORMAT_UNKNOWN;
    bool warnedFormat_ = false;
    std::wstring directory_;
    std::string description_, startTime_;
    uint32_t targetOutputs_ = 0, outputsCaptured_ = 0, inputsCaptured_ = 0;
    int64_t startQpc_ = 0, qpcFreq_ = 1;
    std::atomic<bool> active_{false};

    // Shared with the worker.
    std::mutex mutex_;
    std::condition_variable cvWork_, cvSpace_;
    std::deque<Job> queue_;
    std::deque<std::wstring> finished_;
    bool stop_ = false;
    std::thread worker_;
};

} // namespace

std::unique_ptr<FrameRecorder> FrameRecorder::Create(GpuDevice& device, std::wstring& error) {
    if (!device.Device() || !device.Context()) {
        error = L"FrameRecorder: device not available";
        return nullptr;
    }
    try {
        return std::make_unique<FrameRecorderImpl>(device);
    } catch (const std::exception&) {
        error = L"FrameRecorder: failed to start worker thread";
        return nullptr;
    }
}

} // namespace moonsr
