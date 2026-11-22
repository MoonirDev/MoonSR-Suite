// SPDX-License-Identifier: GPL-3.0-only
#include "moonsr_nr_bridge.h"
#include "pipeline.h"
#include "intel_model.h"
#include "nr_vs.h"
#include "nr_ps.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <iterator>

namespace {
using Microsoft::WRL::ComPtr;
thread_local std::wstring errorText;

MoonSrNrBridgeResult Fail(const wchar_t** error, const std::wstring& message) {
    errorText = message;
    if (error) *error = errorText.c_str();
    return MOONSR_NR_BRIDGE_RUNTIME_FAILURE;
}
std::wstring Widen(const char* text) {
    if (!text) return L"Unknown bridge error";
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (n <= 1) return L"Bridge operation failed";
    std::wstring result(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), n);
    result.pop_back();
    return result;
}
std::wstring ModuleDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module)) return {};
    wchar_t path[32768]{};
    DWORD n = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!n || n >= std::size(path)) return {};
    std::wstring value(path, n);
    auto pos = value.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : value.substr(0, pos);
}
struct Bridge {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> input, output, handoff;
    ComPtr<ID3D11ShaderResourceView> outputSrv;
    ComPtr<ID3D11RenderTargetView> handoffRtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    std::unique_ptr<neural::Pipeline> pipeline;
    // Intel processing adapter: the DLSS5_INTEL runtime replaces the D3D12 pipeline (see intel_model.h).
    bool intel = false;
    std::unique_ptr<neural::IntelModel> intelModel;
    neural::Picture picture;
    ComPtr<ID3D11UnorderedAccessView> outputUav;
    neural::PipelineSettings settings;
    bool rebuild = false;
    unsigned width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ~Bridge() { if (pipeline) pipeline->Retire(); }
    void Resize(unsigned w, unsigned h, DXGI_FORMAT f) {
        if (pipeline && !pipeline->Retire()) throw std::runtime_error("Failed to retire NR pipeline");
        pipeline.reset(); intelModel.reset(); input.Reset(); output.Reset(); handoff.Reset(); outputSrv.Reset();
        handoffRtv.Reset(); outputUav.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = f; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        neural::Check(device->CreateTexture2D(&d, nullptr, &output), "Create NR output");
        neural::Check(device->CreateShaderResourceView(output.Get(), nullptr, &outputSrv), "Create NR output view");
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        neural::Check(device->CreateTexture2D(&d, nullptr, &handoff), "Create BGRA handoff");
        neural::Check(device->CreateRenderTargetView(handoff.Get(), nullptr, &handoffRtv), "Create BGRA handoff view");
        if (!vs) neural::Check(device->CreateVertexShader(g_nr_vs, sizeof(g_nr_vs), nullptr, &vs), "Create handoff vertex shader");
        if (!ps) neural::Check(device->CreatePixelShader(g_nr_ps, sizeof(g_nr_ps), nullptr, &ps), "Create handoff pixel shader");
        if (intel) {
            neural::Check(device->CreateUnorderedAccessView(output.Get(), nullptr, &outputUav), "Create NR output UAV");
            intelModel = std::make_unique<neural::IntelModel>();
            intelModel->Initialize(device.Get(), settings.modelPath, w, h, settings.scalePercent, settings.model,
                                   settings.composition.colour);
        } else {
            pipeline = std::make_unique<neural::Pipeline>();
            pipeline->Initialize(device.Get(), w, h, neural::Encoding::Sdr709, settings);
        }
        rebuild = false;
        width = w; height = h; format = f;
    }
};
bool ValidSettings(const MoonSrNrBridgeConfig& config) {
    return config.passes >= 1 && config.passes <= neural::PipelineSettings::MaxPasses &&
           config.resolutionPercent >= 25 && config.resolutionPercent <= 100 &&
           config.preset <= 2 && config.style <= 2 && config.intensityPercent <= 200 &&
           config.structurePercent <= 200 && config.tonePercent <= 200 && config.skinPercent <= 200 &&
           config.detailPercent <= 400 && config.colourPercent <= 400 && config.autoMask <= 1;
}
void ApplySettings(Bridge& b, const MoonSrNrBridgeConfig& config) {
    auto& s = b.settings;
    // The Intel runtime changes scale live and has one pass and one network, so it never needs a rebuild here.
    b.rebuild = b.rebuild || (!b.intel && (s.scalePercent != config.resolutionPercent ||
                (!s.amd && (s.passes != config.passes || s.model.preset != config.preset))));
    s.passes = config.passes;
    s.scalePercent = config.resolutionPercent;
    s.model.preset = config.preset;
    s.model.style = config.style;
    s.model.intensity = config.intensityPercent / 100.0f;
    s.model.localStructure = config.structurePercent / 100.0f;
    s.model.localTone = config.tonePercent / 100.0f;
    s.model.skinStructure = config.skinPercent / 100.0f;
    s.model.autoMask = config.autoMask != 0;
    s.composition.detail = config.detailPercent / 100.0f;
    s.composition.colour = config.colourPercent / 100.0f;
    if (b.pipeline && !b.rebuild) {
        b.pipeline->SetAppearance(s.model);
        b.pipeline->SetPasses(s.passes);
    }
    if (b.intelModel) {
        b.intelModel->SetScale(s.scalePercent);
        b.intelModel->SetAppearance(s.model, s.composition.colour);
    }
}
MoonSrNrBridgeResult Create(const MoonSrNrBridgeConfig* config, void** instance, const wchar_t** error) {
    if (!config || config->size < sizeof(*config) || config->apiVersion != MOONSR_NR_BRIDGE_API_VERSION ||
        !config->d3d11Device || !config->d3d11Context || !config->modelPath || !instance)
        return Fail(error, L"Invalid neural bridge configuration.");
    *instance = nullptr;
    if (!ValidSettings(*config))
        return Fail(error, L"Invalid neural rendering settings.");
    try {
        auto bridge = std::make_unique<Bridge>();
        bridge->device = static_cast<ID3D11Device*>(config->d3d11Device);
        bridge->context = static_cast<ID3D11DeviceContext*>(config->d3d11Context);
        bridge->settings.modelPath = config->modelPath;
        bridge->settings.amd = config->adapterVendorId == 0x1002;
        bridge->intel = config->adapterVendorId == 0x8086;
        ApplySettings(*bridge, *config);
        const auto dir = ModuleDirectory();
        if (dir.empty()) return Fail(error, L"Cannot locate the neural bridge directory.");
        bridge->settings.forwarderPath = dir + L"\\NeuralRendering_nvngx.dll";
        wchar_t local[32768]{};
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
        bridge->settings.cachePath = n && n < std::size(local) ? std::wstring(local, n) + L"\\MoonSR" : dir;
        *instance = bridge.release();
        return MOONSR_NR_BRIDGE_OK;
    } catch (const std::exception& e) { return Fail(error, Widen(e.what())); }
}
void Destroy(void* instance) { delete static_cast<Bridge*>(instance); }
MoonSrNrBridgeResult Process(void* instance, const MoonSrNrBridgeFrame* frame, MoonSrNrBridgeOutput* out,
                             const wchar_t** error) {
    if (!instance || !frame || frame->size < sizeof(*frame) || !frame->texture || !out || out->size < sizeof(*out) ||
        frame->left < 0 || frame->top < 0 || frame->right <= frame->left || frame->bottom <= frame->top)
        return Fail(error, L"Invalid neural bridge frame.");
    try {
        auto& b = *static_cast<Bridge*>(instance);
        auto* source = static_cast<ID3D11Texture2D*>(frame->texture);
        D3D11_TEXTURE2D_DESC d{}; source->GetDesc(&d);
        const unsigned w = static_cast<unsigned>(frame->right - frame->left);
        const unsigned h = static_cast<unsigned>(frame->bottom - frame->top);
        if (static_cast<unsigned>(frame->right) > d.Width || static_cast<unsigned>(frame->bottom) > d.Height ||
            d.ArraySize != 1 || d.SampleDesc.Count != 1 || d.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            return Fail(error, L"Neural bridge requires a valid SDR BGRA capture.");
        const bool built = b.intel ? b.intelModel != nullptr : b.pipeline != nullptr;
        if (!built || b.rebuild || b.width != w || b.height != h || b.format != d.Format) b.Resize(w, h, d.Format);
        if (frame->reset) {
            if (b.intel) b.intelModel->Invalidate();
            else b.pipeline->Invalidate();
        }
        D3D11_BOX box{static_cast<UINT>(frame->left), static_cast<UINT>(frame->top), 0,
                      static_cast<UINT>(frame->right), static_cast<UINT>(frame->bottom), 1};
        ID3D11Texture2D* original = source;
        const bool canReadDirectly = (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
                                     !(d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS));
        if (box.left || box.top || w != d.Width || h != d.Height || !canReadDirectly) {
            if (!b.input) {
                D3D11_TEXTURE2D_DESC crop{};
                crop.Width = w;
                crop.Height = h;
                crop.MipLevels = crop.ArraySize = crop.SampleDesc.Count = 1;
                crop.Format = d.Format;
                crop.Usage = D3D11_USAGE_DEFAULT;
                crop.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                neural::Check(b.device->CreateTexture2D(&crop, nullptr, &b.input), "Create cropped NR input");
            }
            b.context->CopySubresourceRegion(b.input.Get(), 0, 0, 0, 0, source, 0, &box);
            original = b.input.Get();
        }
        EamRealFrame input{};
        input.context = b.context.Get(); input.original = original; input.output = b.output.Get();
        input.width = w; input.height = h; input.sequence = frame->sequence;
        neural::PictureSettings picture{};
        picture.enabled = frame->pictureEnabled != 0;
        picture.exposure = frame->exposure / 100.0f; picture.contrast = frame->contrast / 100.0f;
        picture.midtones = frame->midtones / 100.0f; picture.shadows = frame->shadows / 100.0f;
        picture.highlights = frame->highlights / 100.0f; picture.saturation = frame->saturation / 100.0f;
        picture.vibrance = frame->vibrance / 100.0f; picture.temperature = frame->temperature / 100.0f;
        picture.tint = frame->tint / 100.0f; picture.sharpen = frame->sharpen / 100.0f;
        ID3D11ShaderResourceView* result = b.outputSrv.Get();
        static const bool trace = GetEnvironmentVariableW(L"MOONSR_NR_TRACE", nullptr, 0) > 0;
        if (b.intel) {
            // The runtime finds motion itself from consecutive frames; the flow guide is not used.
            b.intelModel->Process(b.context.Get(), original, frame->sequence);
            result = b.intelModel->Output();
            if (b.picture.Apply(b.context.Get(), result, b.outputUav.Get(), w, h, neural::Encoding::Sdr709,
                                b.settings.composition.whiteNits, picture.Bounded()))
                result = b.outputSrv.Get();
        } else {
            b.pipeline->Process(input, b.settings.composition, picture.Bounded(),
                                static_cast<ID3D11Texture2D*>(frame->flowGuide));
        }
        if (trace && !b.intel && frame->sequence % 20 == 0) {
            const auto m = b.pipeline->Metrics();
            fprintf(stderr, "NR trace %llu: queue priority %d (capture %d), slot wait %.2f ms, GPU prepare %.2f flow %.2f "
                            "model %.2f compose %.2f ms\n", static_cast<unsigned long long>(frame->sequence),
                    m.nrQueuePriority, m.captureGpuPriority, m.slotWaitMs, m.prepareMs, m.flowMs, m.modelMs, m.compositionMs);
        }
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        b.context->RSSetViewports(1, &vp);
        b.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        b.context->OMSetDepthStencilState(nullptr, 0);
        b.context->RSSetState(nullptr);
        auto* target = b.handoffRtv.Get();
        b.context->OMSetRenderTargets(1, &target, nullptr);
        b.context->IASetInputLayout(nullptr);
        b.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        b.context->VSSetShader(b.vs.Get(), nullptr, 0);
        b.context->GSSetShader(nullptr, nullptr, 0);
        b.context->PSSetShader(b.ps.Get(), nullptr, 0);
        auto* view = result;
        b.context->PSSetShaderResources(0, 1, &view);
        b.context->Draw(3, 0);
        ID3D11ShaderResourceView* noView = nullptr;
        b.context->PSSetShaderResources(0, 1, &noView);
        ID3D11RenderTargetView* noTarget = nullptr;
        b.context->OMSetRenderTargets(1, &noTarget, nullptr);
        out->texture = b.handoff.Get(); out->left = out->top = 0;
        out->right = static_cast<int32_t>(w); out->bottom = static_cast<int32_t>(h);
        return MOONSR_NR_BRIDGE_OK;
    } catch (const std::exception& e) { return Fail(error, Widen(e.what())); }
}
const MoonSrNrBridgeApi api{sizeof(api), MOONSR_NR_BRIDGE_API_VERSION, Create, Destroy, Process};
}
extern "C" MOONSR_NR_BRIDGE_EXPORT const MoonSrNrBridgeApi* MoonSR_NrBridge_GetApi(uint32_t version) {
    return version == MOONSR_NR_BRIDGE_API_VERSION ? &api : nullptr;
}
extern "C" MOONSR_NR_BRIDGE_EXPORT MoonSrNrBridgeResult MoonSR_NrBridge_UpdateSettings(
    void* instance, const MoonSrNrBridgeConfig* settings, const wchar_t** error) {
    if (!instance || !settings || settings->size < sizeof(*settings) ||
        settings->apiVersion != MOONSR_NR_BRIDGE_API_VERSION || !ValidSettings(*settings))
        return Fail(error, L"Invalid live neural rendering settings.");
    ApplySettings(*static_cast<Bridge*>(instance), *settings);
    return MOONSR_NR_BRIDGE_OK;
}
