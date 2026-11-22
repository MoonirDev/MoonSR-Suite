// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "gpu.h"
#include "model.h"
#include "third_party/amd_nr/LmxxfNrApi.h"

namespace neural {
// Runs one AMDNR job per output frame on the capture adapter's D3D12 queue.
class AmdModel {
  public:
    ~AmdModel();
    void Initialize(Gpu&, const std::wstring& runtimePath);
    void RecordInput(Recording&, Resource&, const ModelSettings&, unsigned passes);
    void Enqueue(Gpu&);
    Resource RecordOutput(Recording&);
    void RetireSubmitted();

  private:
    std::runtime_error Error(const char* operation, int32_t result) const;
    HMODULE module_ = nullptr;
    LmxxfNrApi api_{};
    void* context_ = nullptr;
    LmxxfNrJob job_{};
    uint64_t sequence_ = 0;
    bool launched_ = false;
    uint32_t maxWidth_ = 0, maxHeight_ = 0;
};
}
