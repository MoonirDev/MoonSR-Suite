// SPDX-License-Identifier: GPL-3.0-only
#include "amd_model.h"

#include <cstdio>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        // The test runtime issues no HIP work and can run with a local NVIDIA D3D12 adapter.
        neural::Gpu gpu;
        gpu.Initialize();
        auto input = gpu.Texture(320, 240, DXGI_FORMAT_R16G16B16A16_FLOAT);
        neural::Recording producers[2], consumers[2];
        const auto path = std::filesystem::absolute(argv[1]).wstring();
        const HMODULE fixture = LoadLibraryW(path.c_str());
        if (!fixture) throw std::runtime_error("Load AMDNR fixture");
        const auto setStrict = reinterpret_cast<void(*)()>(GetProcAddress(fixture, "MoonSrAmdNrFixture_SetStrict"));
        if (!setStrict) throw std::runtime_error("Find fixture ABI check export");
        setStrict();
        neural::AmdModel model;
        model.Initialize(gpu, path);
        neural::ModelSettings settings;
        settings.localStructure = settings.skinStructure = 1.f;
        for (unsigned frame = 0; frame < 2; ++frame) {
            auto& producer = producers[frame];
            auto& consumer = consumers[frame];
            producer.Initialize(gpu.device.Get());
            consumer.Initialize(gpu.device.Get());
            producer.Begin(gpu.fence.Get());
            input.Transition(producer.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (frame) {
                settings.autoMask = false;
                settings.localStructure = .5f;
                settings.preset = 1;
            }
            model.RecordInput(producer, input, settings, frame ? 10 : 1);
            gpu.Submit(producer);
            model.Enqueue(gpu);
            consumer.Begin(gpu.fence.Get());
            auto output = model.RecordOutput(consumer);
            if (output.object.Get() != input.object.Get() || output.width != input.width ||
                output.height != input.height || output.state != input.state)
                throw std::runtime_error("Invalid fixture output");
            gpu.Submit(consumer);
            model.RetireSubmitted();
        }
        if (!gpu.Wait(gpu.LastSubmitted())) throw std::runtime_error("Fixture queue did not drain");
        puts("AMDNR frame ABI smoke passed with the fixture");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "AMDNR frame ABI smoke failed: %s\n", error.what());
        return 1;
    }
}
