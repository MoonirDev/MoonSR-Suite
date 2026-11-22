// SPDX-License-Identifier: GPL-3.0-only
// Test stand-in for the DLSS5_INTEL runtime (d5c_chain.dll). Same C ABI as dlss5/chain/chain_c_api.h.
// dlss5_process_frame writes the colour inverse of its input, so a test can check the whole host -> bridge ->
// runtime -> bridge path bit-exactly. dlss5_create also enumerates the Vulkan devices visible to this process
// (through the system vulkan-1.dll, minimal hand-written declarations, no SDK) so a test can see the effect of the
// bridge's VK_LOADER_DRIVERS_SELECT filter. Export intel_fixture_report returns what it saw.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
struct State {
    float scale = 1, intensity = 0, tone = 0, structure = 0, skin = 0, colour = 0;
    int autoMask = 0, style = 0;
    unsigned resets = 0, frames = 0;
    std::string weights, shaders, devices;
};
std::string g_report;
State* g_last = nullptr; // most recent runtime context, for intel_fixture_report

// Minimal Vulkan 1.0 declarations for instance creation and physical-device listing.
struct VkApplicationInfo { int sType; const void* pNext; const char* name; uint32_t version; const char* engine;
                           uint32_t engineVersion; uint32_t apiVersion; };
struct VkInstanceCreateInfo { int sType; const void* pNext; uint32_t flags; const VkApplicationInfo* app;
                              uint32_t layers; const char* const* layerNames; uint32_t exts; const char* const* extNames; };
using VkInstance = void*;
using VkPhysicalDevice = void*;
using CreateInstance = int(__stdcall*)(const VkInstanceCreateInfo*, const void*, VkInstance*);
using DestroyInstance = void(__stdcall*)(VkInstance, const void*);
using EnumerateDevices = int(__stdcall*)(VkInstance, uint32_t*, VkPhysicalDevice*);
using GetProperties = void(__stdcall*)(VkPhysicalDevice, void*);

std::string ListVulkanDevices() {
    HMODULE vk = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!vk) return "no vulkan-1.dll";
    auto create = reinterpret_cast<CreateInstance>(GetProcAddress(vk, "vkCreateInstance"));
    auto destroy = reinterpret_cast<DestroyInstance>(GetProcAddress(vk, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<EnumerateDevices>(GetProcAddress(vk, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<GetProperties>(GetProcAddress(vk, "vkGetPhysicalDeviceProperties"));
    std::string out;
    if (create && destroy && enumerate && properties) {
        VkApplicationInfo app{0 /*VK_STRUCTURE_TYPE_APPLICATION_INFO*/, nullptr, "intel_fixture", 1, "", 0, (1u << 22)};
        VkInstanceCreateInfo info{1 /*VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO*/, nullptr, 0, &app, 0, nullptr, 0, nullptr};
        VkInstance instance = nullptr;
        const int r = create(&info, nullptr, &instance);
        if (r != 0) {
            out = "vkCreateInstance " + std::to_string(r);
        } else {
            uint32_t n = 0;
            enumerate(instance, &n, nullptr);
            VkPhysicalDevice devices[16]{};
            n = n > 16 ? 16 : n;
            enumerate(instance, &n, devices);
            out = std::to_string(n) + " device(s)";
            for (uint32_t i = 0; i < n; ++i) {
                alignas(8) unsigned char props[2048]{}; // VkPhysicalDeviceProperties is 824 bytes
                properties(devices[i], props);
                uint32_t vendor = 0;
                std::memcpy(&vendor, props + 8, 4); // apiVersion, driverVersion, vendorID
                char line[320];
                std::snprintf(line, sizeof(line), "; %04x %s", vendor, reinterpret_cast<const char*>(props + 20));
                out += line;
            }
            destroy(instance, nullptr);
        }
    } else {
        out = "vulkan-1.dll lacks entry points";
    }
    FreeLibrary(vk);
    return out;
}
} // namespace

extern "C" {
__declspec(dllexport) void* dlss5_create(const char* weights, const char* shaders, float scale, float, float) {
    auto* s = new State;
    s->weights = weights ? weights : "";
    s->shaders = shaders ? shaders : "";
    s->scale = scale;
    s->devices = ListVulkanDevices();
    g_last = s;
    return s;
}
__declspec(dllexport) void dlss5_destroy(void* h) {
    if (h == g_last) g_last = nullptr;
    delete static_cast<State*>(h);
}
__declspec(dllexport) int dlss5_process_frame(void* h, uint32_t w, uint32_t hgt, const void* in, void* out, long) {
    if (!h || !in || !out || !w || !hgt) return -1;
    const auto* src = static_cast<const uint8_t*>(in);
    auto* dst = static_cast<uint8_t*>(out);
    for (size_t i = 0, n = size_t(w) * hgt * 4; i < n; i += 4) {
        dst[i] = uint8_t(255 - src[i]); dst[i + 1] = uint8_t(255 - src[i + 1]); dst[i + 2] = uint8_t(255 - src[i + 2]);
        dst[i + 3] = 255;
    }
    ++static_cast<State*>(h)->frames;
    return 0;
}
__declspec(dllexport) void dlss5_set_scale(void* h, float scale) { if (h) static_cast<State*>(h)->scale = scale; }
__declspec(dllexport) void dlss5_set_phase30(void* h, float intensity, float tone, float structure, float skin,
                                             int autoMask, int style, float colour) {
    if (!h) return;
    auto* s = static_cast<State*>(h);
    s->intensity = intensity; s->tone = tone; s->structure = structure; s->skin = skin;
    s->autoMask = autoMask; s->style = style; s->colour = colour;
}
__declspec(dllexport) void dlss5_reset_history(void* h) { if (h) ++static_cast<State*>(h)->resets; }
// Not part of the runtime ABI: state of the most recent live runtime context, for the test.
__declspec(dllexport) const char* intel_fixture_report() {
    void* h = g_last;
    if (!h) return "";
    const auto* s = static_cast<State*>(h);
    char line[512];
    std::snprintf(line, sizeof(line), "frames %u resets %u scale %.2f intensity %.2f tone %.2f structure %.2f skin %.2f "
                  "mask %d style %d colour %.2f | ", s->frames, s->resets, s->scale, s->intensity, s->tone,
                  s->structure, s->skin, s->autoMask, s->style, s->colour);
    g_report = line + s->devices + " | weights " + s->weights + " | shaders " + s->shaders;
    return g_report.c_str();
}
}
