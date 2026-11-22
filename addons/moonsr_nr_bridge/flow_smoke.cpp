// SPDX-License-Identifier: GPL-3.0-only
// Standalone Optical Flow direction/scale test. No NR model or compositor is loaded.
#include "bidirectional_flow.h"
#include "flow_shared.h"
#include "flow_confidence_test.h"
#include "flow_input.h"
#include "hardware_flow_analysis.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    try {
        bool full = false, fast = false, hints = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--1080p") full = true;
            else if (std::string_view(argv[i]) == "--fast") fast = true;
            else if (std::string_view(argv[i]) == "--hints") hints = true;
            else throw std::runtime_error("Usage: MoonSR_FlowSmoke.exe [--1080p] [--fast] [--hints]");
        }
        const uint32_t w = full ? 1920 : 640, h = full ? 1080 : 480;
        constexpr int dx = 12, dy = 8;
        neural::Gpu gpu;
        gpu.Initialize();
        printf("Adapter: %ls, input %ux%u, preset %s\n", gpu.adapterName.c_str(), w, h, fast ? "fast" : "medium");
        moonsr::BidirectionalFlow flow;
        flow.Initialize(gpu, w, h, fast ? NV_OF_PERF_LEVEL_FAST : NV_OF_PERF_LEVEL_MEDIUM);
        bool rejected = false;
        try { flow.Grid(0); } catch (const std::runtime_error&) { rejected = true; }
        if (!rejected) throw std::runtime_error("Flow exposed an uninitialized field");
        neural::Recording rec;
        rec.Initialize(gpu.device.Get());
        neural::ComPtr<ID3D11Device> device11;
        neural::ComPtr<ID3D11DeviceContext> context11;
        neural::Check(D3D11CreateDevice(gpu.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                      D3D11_SDK_VERSION, &device11, nullptr, &context11), "Create sharing test device");
        moonsr::SharedFlowInputs inputs;
        inputs.Initialize(gpu,device11.Get(),w,h);
        D3D11_TEXTURE2D_DESC sourceDesc{};
        sourceDesc.Width=w; sourceDesc.Height=h; sourceDesc.MipLevels=1; sourceDesc.ArraySize=1;
        sourceDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sourceDesc.SampleDesc.Count=1;
        neural::ComPtr<ID3D11Texture2D> source11;
        neural::Check(device11->CreateTexture2D(&sourceDesc,nullptr,&source11),"Create capture fixture");
        auto pixel = [](int x, int y) {
            // Smooth textured cells provide trackable structure without a repeating checkerboard.
            unsigned q = unsigned(x / 4 + 1000) * 1664525u ^ unsigned(y / 4 + 1000) * 1013904223u;
            q ^= q >> 13; q *= 2246822519u; q ^= q >> 16;
            return 0xff000000u | (q & 0x00ffffffu);
        };
        for (uint32_t slot = 0; slot < 2; ++slot) {
            auto& image = inputs.Image(slot);
            std::vector<uint32_t> pixels(size_t(w)*h);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    pixels[y * w + x] = pixel(int(x) - (slot ? dx : 0), int(y) - (slot ? dy : 0));
            context11->UpdateSubresource(source11.Get(),0,nullptr,pixels.data(),w*4,0);
            inputs.Copy(gpu,source11.Get(),slot);
            rec.Begin(gpu.fence.Get());
            flow.Prepare(gpu, rec, image, slot);
            if (!gpu.Wait(gpu.Submit(rec))) throw std::runtime_error("Input submission timed out");
        }
        flow.Execute(gpu, 1, 0, false, gpu.LastSubmitted());
        moonsr::SharedFlowFields shared;
        shared.Initialize(gpu, device11.Get(), flow.Grid(0).width, flow.Grid(0).height);
        std::array<neural::Recording,3> publications;
        for (auto& publication:publications) {
            publication.Initialize(gpu.device.Get());
            publication.Begin(gpu.fence.Get());
            flow.Convert(gpu, publication, inputs.Image(0), inputs.Image(1), {&shared.Output(0), &shared.Output(1)});
            for (unsigned slot=0;slot<2;++slot) inputs.Image(slot).Transition(publication.list.Get(),D3D12_RESOURCE_STATE_COMMON);
            shared.Publish(gpu, publication);
            inputs.MarkConsumed(gpu,gpu.LastSubmitted());
        }
        D3D11_TEXTURE2D_DESC sharedDesc{};
        shared.Array()->GetDesc(&sharedDesc);
        sharedDesc.Usage=D3D11_USAGE_STAGING; sharedDesc.BindFlags=0; sharedDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        neural::ComPtr<ID3D11Texture2D> staging11;
        neural::Check(device11->CreateTexture2D(&sharedDesc,nullptr,&staging11), "Create shared flow readback");
        context11->CopyResource(staging11.Get(),shared.Array());
        for (unsigned d=0; d<2; ++d) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            neural::Check(context11->Map(staging11.Get(),d,D3D11_MAP_READ,0,&mapped), "Read D3D11 flow array");
            auto centre=reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped.pData)+(sharedDesc.Height/2)*mapped.RowPitch)+(sharedDesc.Width/2)*4;
            const float error=std::hypot(centre[0]-(d ? -dx : dx),centre[1]-(d ? -dy : dy));
            context11->Unmap(staging11.Get(),d);
            if (!std::isfinite(error) || error>2) throw std::runtime_error("Shared flow array direction mismatch");
        }
        puts("PASS: D3D12 conversion -> shared texture -> D3D11 two-slice array");
        for (uint32_t direction = 0; direction < 2; ++direction) {
            auto& grid = shared.Output(direction);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 bytes = 0;
            const auto desc = grid.object->GetDesc();
            gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
            rec.Begin(gpu.fence.Get());
            grid.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = grid.object.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = readback.object.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = footprint;
            rec.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            grid.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_COMMON);
            if (!gpu.Wait(gpu.Submit(rec))) throw std::runtime_error("Flow readback timed out");
            void* mapped = nullptr;
            neural::Check(readback.object->Map(0, nullptr, &mapped), "Map flow");
            std::vector<float> xs, ys, errors;
            const float ex = direction ? -float(dx) : float(dx), ey = direction ? -float(dy) : float(dy);
            double maxResidualError = 0, maxVarianceError = 0;
            auto luminance = [&](int x, int y, unsigned frame) {
                x = std::clamp(x, 0, int(w)-1); y = std::clamp(y, 0, int(h)-1);
                const uint32_t rgb = pixel(x - (frame ? dx : 0), y - (frame ? dy : 0));
                return .2126 * double(rgb & 255) + .7152 * double((rgb >> 8) & 255) + .0722 * double((rgb >> 16) & 255);
            };
            // Sparse independent CPU evaluation covers interior and edge windows. Sampling operates in pixel
            // coordinates (texel centres at integers), matching SampleLevel's normalized-UV convention.
            for (uint32_t cy = 0; cy < grid.height; cy += 13) {
                auto row = reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped) + footprint.Offset + cy * footprint.Footprint.RowPitch);
                for (uint32_t cx = 0; cx < grid.width; cx += 17) {
                    const double vx = row[cx*4], vy = row[cx*4+1];
                    double sd = 0, sd2 = 0, sum = 0, sum2 = 0;
                    for (int oy = 0; oy < 6; ++oy) for (int ox = 0; ox < 6; ++ox) {
                        const int px = int(cx)*4-1+ox, py = int(cy)*4-1+oy;
                        const double a = luminance(px, py, direction);
                        const double rx = px+vx, ry = py+vy;
                        const int ix = int(std::floor(rx)), iy = int(std::floor(ry));
                        const double fx = rx-ix, fy = ry-iy;
                        const unsigned reference = 1-direction;
                        const double b = (1-fy)*((1-fx)*luminance(ix,iy,reference)+fx*luminance(ix+1,iy,reference)) +
                            fy*((1-fx)*luminance(ix,iy+1,reference)+fx*luminance(ix+1,iy+1,reference));
                        const double d = a-b;
                        sd += d; sd2 += d*d; sum += a; sum2 += a*a;
                    }
                    const double rms = std::sqrt(std::max(sd2-sd*sd/36.,0.)/36.);
                    const double stdev = std::sqrt(std::max(sum2/36.-(sum/36.)*(sum/36.),0.));
                    if (!std::isfinite(row[cx*4+2]) || !std::isfinite(row[cx*4+3]))
                        throw std::runtime_error("Nonfinite sampled metrics");
                    maxResidualError = std::max(maxResidualError, std::abs(rms-row[cx*4+2]));
                    maxVarianceError = std::max(maxVarianceError, std::abs(stdev-row[cx*4+3]));
                }
            }
            printf("direction %u: CPU reference max residual error %.5f, stddev error %.5f luminance levels\n",
                   direction, maxResidualError, maxVarianceError);
            if (maxResidualError > .1 || maxVarianceError > .1)
                throw std::runtime_error("Converted metrics differ from CPU reference");
            for (uint32_t y = 8; y + 8 < grid.height; ++y) {
                auto row = reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch);
                for (uint32_t x = 8; x + 8 < grid.width; ++x) {
                    const float vx = row[x * 4], vy = row[x * 4 + 1];
                    if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(row[x*4+2]) ||
                        !std::isfinite(row[x*4+3]) || row[x*4+2] < 0 || row[x*4+3] <= 0)
                        throw std::runtime_error("Invalid converted motion metrics");
                    xs.push_back(vx); ys.push_back(vy);
                    errors.push_back(std::hypot(vx - ex, vy - ey));
                }
            }
            readback.object->Unmap(0, nullptr);
            auto percentile = [](std::vector<float>& v, float fraction) {
                std::sort(v.begin(), v.end()); return v[size_t(fraction * float(v.size() - 1))];
            };
            const float p95 = percentile(errors, .95f);
            printf("direction %u: median (%.3f, %.3f), expected (%.0f, %.0f), p95 error %.3f px\n",
                   direction, percentile(xs, .5f), percentile(ys, .5f), ex, ey, p95);
            if (p95 > 2.0f) throw std::runtime_error("Optical Flow translation check failed");
        }
        {
            neural::ComPtr<ID3D11Texture2D> previous11;
            neural::Check(device11->CreateTexture2D(&sourceDesc,nullptr,&previous11),"Create backend previous input");
            std::vector<uint32_t> pixels(size_t(w)*h);
            for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x) pixels[y*w+x]=pixel(x,y);
            context11->UpdateSubresource(previous11.Get(),0,nullptr,pixels.data(),w*4,0);
            moonsr::HardwareFlowAnalysis backend(device11.Get(),w,h,sourceDesc.Format,hints);
            for (unsigned repeat=0;repeat<12;++repeat) {
                for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x)
                    pixels[y*w+x]=pixel(int(x)-int(repeat)*dx,int(y)-int(repeat)*dy);
                context11->UpdateSubresource(previous11.Get(),0,nullptr,pixels.data(),w*4,0);
                for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x)
                    pixels[y*w+x]=pixel(int(x)-int(repeat+1)*dx,int(y)-int(repeat+1)*dy);
                context11->UpdateSubresource(source11.Get(),0,nullptr,pixels.data(),w*4,0);
                const uint64_t sequence=repeat+2+(repeat>=6 ? 4 : 0);
                auto srv=backend.Analyse(previous11.Get(),source11.Get(),sequence);
                neural::ComPtr<ID3D11Resource> resource;
                srv->GetResource(&resource);
                context11->CopyResource(staging11.Get(),resource.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                neural::Check(context11->Map(staging11.Get(),0,D3D11_MAP_READ,0,&mapped),"Read composed hardware backend");
                auto v=reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped.pData)+(sharedDesc.Height/2)*mapped.RowPitch)+(sharedDesc.Width/2)*4;
                const float error=std::hypot(v[0]-dx,v[1]-dy);
                context11->Unmap(staging11.Get(),0);
                if (!std::isfinite(error)||error>2) throw std::runtime_error("Composed hardware backend failed");
            }
            puts("PASS: composed hardware motion provider, 12 moving pairs including a sequence discontinuity");
        }
        CheckFlowConfidence(device11.Get(),context11.Get(),shared,w,h);
        // Repeated known pairs measure the driver/fence path only, excluding input packing and upload.
        // Keep hints disabled: identical repeated pairs are not a temporal game sequence.
        std::vector<double> times;
        for (unsigned i = 0; i < 110; ++i) {
            const auto start = std::chrono::steady_clock::now();
            flow.Execute(gpu, 1, 0, false, gpu.LastSubmitted());
            if (!gpu.Wait(gpu.LastSubmitted())) throw std::runtime_error("Bidirectional timing timed out");
            if (i >= 10) times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        printf("Two independent sessions, 100 warmed pairs: wall median %.3f ms, p95 %.3f ms (includes CPU submission/fence wait; excludes packing/conversion)\n",
               times[times.size() / 2], times[size_t(.95 * (times.size() - 1))]);
        flow.Reset();
        rejected = false;
        try { flow.Grid(0); } catch (const std::runtime_error&) { rejected = true; }
        if (!rejected) throw std::runtime_error("Flow exposed a field after reset");
        if (!flow.Shutdown()) throw std::runtime_error("Optical Flow shutdown timed out");
        puts("PASS: bidirectional driver flow, fixed-point scale, GPU fences, and shutdown");
        return 0;
    } catch (const std::exception& e) { fprintf(stderr, "%s\n", e.what()); return 1; }
}
