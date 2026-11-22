// SPDX-License-Identifier: GPL-3.0-only
// Test runtime that validates the frame ABI and the producer/consumer call sequence.
#define LMXXF_NR_RUNTIME_EXPORTS
#include "third_party/amd_nr/LmxxfNrApi.h"

#include <new>

namespace {
bool strict = false;
struct Context {
    void* queue;
    void* producer = nullptr;
    unsigned phase = 0;
    uint64_t sequence = 0;
};
int32_t Caps(LmxxfNrCapabilities* caps) {
    *caps = {sizeof(*caps), LMXXF_NR_ABI_VERSION, 1920, 1080, 0, 0, 0, 1, 0};
    return LMXXF_NR_OK;
}
int32_t Create(const LmxxfNrCreateInfo* info, void** out) {
    if (info->struct_size != sizeof(*info) || !info->device || !info->queue || !info->assets_directory)
        return LMXXF_NR_INVALID_ARGUMENT;
    *out = new (std::nothrow) Context{info->queue};
    return *out ? LMXXF_NR_OK : LMXXF_NR_FAILED;
}
int32_t Destroy(void* context) {
    auto* ctx = static_cast<Context*>(context);
    const bool idle = ctx->phase == 0;
    delete ctx;
    return idle ? LMXXF_NR_OK : LMXXF_NR_FAILED;
}
int32_t Prepare(void* context, const LmxxfNrFrameInfo* frame, LmxxfNrJob* job) {
    auto& ctx = *static_cast<Context*>(context);
    const bool first = ctx.sequence == 0;
    const uint32_t flags = LMXXF_NR_FRAME_FLAG_STRENGTH |
                           (first ? 0u : LMXXF_NR_FRAME_FLAG_CONTROLS | LMXXF_NR_FRAME_FLAG_FULL_NETWORK);
    if (ctx.phase != 0 || frame->frame_id != ctx.sequence + 1 || frame->list_generation != frame->frame_id ||
        frame->passes < 1 || frame->passes > 3 || !frame->color || !frame->command_list ||
        !frame->color_width || !frame->color_height ||
        (frame->flags & LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH))
        return LMXXF_NR_INVALID_ARGUMENT;
    if (strict && (frame->struct_size != (first ? 128u : sizeof(*frame)) || frame->flags != flags ||
                   frame->passes != (first ? 1u : 3u) ||
                   (!first && (frame->control_skin != -1.f || frame->control_structure != .5f))))
        return LMXXF_NR_INVALID_ARGUMENT;
    ctx.producer = frame->command_list;
    ++ctx.sequence;
    ctx.phase = 1;
    job->handle = context;
    // Identity output. The real runtime would provide its own resource in the same state.
    job->private_output = frame->color;
    return LMXXF_NR_OK;
}
int32_t RecordInputs(void* context, void* job, void* list) {
    auto& ctx = *static_cast<Context*>(context);
    if (context != job || ctx.phase != 1 || ctx.producer != list) return LMXXF_NR_INVALID_ARGUMENT;
    ctx.phase = 2;
    return LMXXF_NR_OK;
}
int32_t Enqueue(void* context, void* job, void* queue) {
    auto& ctx = *static_cast<Context*>(context);
    if (context != job || ctx.phase != 2 || ctx.queue != queue) return LMXXF_NR_INVALID_ARGUMENT;
    ctx.phase = 3;
    return LMXXF_NR_OK;
}
int32_t RecordOutputs(void* context, void* job, void* list) {
    auto& ctx = *static_cast<Context*>(context);
    if (context != job || ctx.phase != 3 || !list || ctx.producer == list)
        return LMXXF_NR_INVALID_ARGUMENT;
    ctx.phase = 4;
    return LMXXF_NR_OK;
}
int32_t Retire(void* context, void* job) {
    auto& ctx = *static_cast<Context*>(context);
    if (context != job || ctx.phase != 4) return LMXXF_NR_INVALID_ARGUMENT;
    ctx.phase = 0;
    return LMXXF_NR_OK;
}
int32_t Cancel(void* context, void*) {
    static_cast<Context*>(context)->phase = 0;
    return LMXXF_NR_OK;
}
}

extern "C" __declspec(dllexport) void MoonSrAmdNrFixture_SetStrict() { strict = true; }

extern "C" LMXXF_NR_EXPORT int32_t LmxxfNrGetApi(uint32_t version, LmxxfNrApi* out) {
    if (version != LMXXF_NR_ABI_VERSION || !out || out->struct_size != sizeof(*out))
        return LMXXF_NR_UNSUPPORTED_ABI;
    out->abi_version = LMXXF_NR_ABI_VERSION;
    out->QueryCapabilities = Caps;
    out->Create = Create;
    out->Destroy = Destroy;
    out->PrepareSession = [](void*) -> int32_t { return LMXXF_NR_OK; };
    out->PrepareFrame = Prepare;
    out->RecordInputs = RecordInputs;
    out->EnqueueHipAsync = Enqueue;
    out->RecordOutputs = RecordOutputs;
    out->Retire = Retire;
    out->CancelUnsubmitted = Cancel;
    out->AbandonJob = Cancel;
    out->Drain = [](void*) -> int32_t { return LMXXF_NR_OK; };
    return LMXXF_NR_OK;
}
