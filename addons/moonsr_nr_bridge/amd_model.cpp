// SPDX-License-Identifier: GPL-3.0-only
#include "amd_model.h"
#include "third_party/amd_nr/NrControls.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>

namespace neural {
AmdModel::~AmdModel() {
    if (context_) {
        if (job_.handle) {
            if (launched_) api_.AbandonJob(context_, job_.handle);
            else api_.CancelUnsubmitted(context_, job_.handle);
        }
        api_.Drain(context_);
        // Keep the module mapped if the runtime could not retire its context.
        if (api_.Destroy(context_) != LMXXF_NR_OK) return;
    }
    if (module_) FreeLibrary(module_);
}

std::runtime_error AmdModel::Error(const char* operation, int32_t result) const {
    char message[1024]{};
    if (api_.GetLastError) api_.GetLastError(message, sizeof(message));
    return std::runtime_error(std::string("AMDNR ") + operation + " (" + std::to_string(result) + ")" +
                              (message[0] ? std::string(": ") + message : ""));
}

void AmdModel::Initialize(Gpu& gpu, const std::wstring& runtimePath) {
    const auto assets = (std::filesystem::path(runtimePath).parent_path() / L"LmxxfNrRuntime.pak").wstring();
    module_ = LoadLibraryExW(runtimePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) throw std::runtime_error("Cannot load LmxxfNrRuntime.dll or its dependencies (Windows error " +
                                           std::to_string(GetLastError()) + ")");
    const auto getApi = reinterpret_cast<decltype(&LmxxfNrGetApi)>(GetProcAddress(module_, "LmxxfNrGetApi"));
    api_.struct_size = sizeof(api_);
    if (!getApi || getApi(LMXXF_NR_ABI_VERSION, &api_) != LMXXF_NR_OK ||
        api_.abi_version != LMXXF_NR_ABI_VERSION || !api_.QueryCapabilities || !api_.Create || !api_.Destroy ||
        !api_.PrepareSession || !api_.PrepareFrame || !api_.RecordInputs || !api_.EnqueueHipAsync ||
        !api_.RecordOutputs || !api_.Retire || !api_.CancelUnsubmitted || !api_.AbandonJob || !api_.Drain)
        throw std::runtime_error("Incompatible AMDNR runtime. Use the matched AMDNR DLL and model pack.");

    LmxxfNrCreateInfo info{sizeof(info), gpu.device.Get(), gpu.queue.Get(), assets.c_str(), 0};
    const auto created = api_.Create(&info, &context_);
    if (created != LMXXF_NR_OK || !context_) throw Error("Create", created);
    const auto prepared = api_.PrepareSession(context_);
    if (prepared != LMXXF_NR_OK) throw Error("PrepareSession", prepared);
    if (const auto tier = reinterpret_cast<decltype(&LmxxfNrSetTierPolicy)>(
            GetProcAddress(module_, "LmxxfNrSetTierPolicy"))) {
        const auto result = tier(context_, 1);
        if (result != LMXXF_NR_OK) throw Error("SetTierPolicy", result);
    }
    LmxxfNrCapabilities caps{};
    caps.struct_size = sizeof(caps);
    const auto queried = api_.QueryCapabilities(&caps);
    if (queried != LMXXF_NR_OK) throw Error("QueryCapabilities", queried);
    maxWidth_ = caps.max_input_width;
    maxHeight_ = caps.max_input_height;
}

void AmdModel::RecordInput(Recording& rec, Resource& colour, const ModelSettings& settings, unsigned passes) {
    if ((maxWidth_ && colour.width > maxWidth_) || (maxHeight_ && colour.height > maxHeight_))
        throw std::runtime_error("AMDNR working size exceeds the runtime limit. Lower Working resolution.");
    LmxxfNrFrameInfo frame{};
    frame.struct_size = offsetof(LmxxfNrFrameInfo, control_tone) + sizeof(float); // ABI 1, 128-byte layout
    frame.session_id = 1;
    frame.frame_id = frame.list_generation = ++sequence_;
    frame.command_list = rec.list.Get();
    frame.color = colour.object.Get();
    frame.color_width = colour.width;
    frame.color_height = colour.height;
    frame.color_state = colour.state;
    frame.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
    if (settings.preset == 1) frame.flags |= LMXXF_NR_FRAME_FLAG_FULL_NETWORK;
    frame.transfer_strength = frame.color_strength = frame.model_scale = 1.f;
    frame.passes = std::min(passes, 3u);
    const auto controls = AmdPreSr::ResolveNrControls(settings.autoMask, settings.localStructure, settings.skinStructure);
    if (!controls.BuiltIn()) {
        frame.struct_size = sizeof(frame);
        frame.flags |= LMXXF_NR_FRAME_FLAG_CONTROLS;
        AmdPreSr::WriteNrControls(frame, controls);
    }
    job_ = {};
    job_.struct_size = sizeof(job_);
    const auto prepared = api_.PrepareFrame(context_, &frame, &job_);
    if (prepared != LMXXF_NR_OK || !job_.handle) throw Error("PrepareFrame", prepared);
    const auto recorded = api_.RecordInputs(context_, job_.handle, rec.list.Get());
    if (recorded != LMXXF_NR_OK) throw Error("RecordInputs", recorded);
}

void AmdModel::Enqueue(Gpu& gpu) {
    const auto result = api_.EnqueueHipAsync(context_, job_.handle, gpu.queue.Get());
    if (result != LMXXF_NR_OK) throw Error("EnqueueHipAsync", result);
    launched_ = true;
}

Resource AmdModel::RecordOutput(Recording& rec) {
    const auto result = api_.RecordOutputs(context_, job_.handle, rec.list.Get());
    if (result != LMXXF_NR_OK) throw Error("RecordOutputs", result);
    Resource output;
    output.object = static_cast<ID3D12Resource*>(job_.private_output);
    if (!output.object) throw std::runtime_error("AMDNR did not provide an output texture");
    const auto desc = output.object->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1)
        throw std::runtime_error("AMDNR returned an incompatible output texture");
    output.format = desc.Format;
    output.width = static_cast<uint32_t>(desc.Width);
    output.height = desc.Height;
    output.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return output;
}

void AmdModel::RetireSubmitted() {
    const auto result = api_.Retire(context_, job_.handle);
    if (result != LMXXF_NR_OK) throw Error("Retire", result);
    job_ = {};
    launched_ = false;
}
}
