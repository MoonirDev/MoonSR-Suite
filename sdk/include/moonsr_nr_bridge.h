// MoonSR Neural Rendering bridge ABI, version 4.
//
// The bridge is a separately built, user-installed DLL. MoonSR deliberately does not carry vendor
// SDK headers, runtimes, or an implementation of an undocumented vendor ABI. The bridge receives
// stage input frames plus an original-capture motion guide and returns an enhanced D3D11 texture.
#pragma once

#include <stdint.h>

#ifdef _WIN32
#define MOONSR_NR_BRIDGE_EXPORT __declspec(dllexport)
#else
#define MOONSR_NR_BRIDGE_EXPORT
#endif

#define MOONSR_NR_BRIDGE_API_VERSION 4u

typedef enum MoonSrNrBridgeResult {
    MOONSR_NR_BRIDGE_OK = 0,
    MOONSR_NR_BRIDGE_UNSUPPORTED = 1,
    MOONSR_NR_BRIDGE_INVALID_ARGUMENT = 2,
    MOONSR_NR_BRIDGE_RUNTIME_FAILURE = 3,
} MoonSrNrBridgeResult;

typedef struct MoonSrNrBridgeConfig {
    uint32_t size;
    uint32_t apiVersion;
    // Borrowed pointers. `d3d11Device` and `d3d11Context` are ID3D11Device1 and ID3D11DeviceContext1.
    void* d3d11Device;
    void* d3d11Context;
    const wchar_t* modelPath; // NVIDIA model DLL, or AMDNR runtime DLL beside its model pack
    uint32_t adapterVendorId;
    uint32_t passes, resolutionPercent, preset, style;
    uint32_t intensityPercent, structurePercent, tonePercent, skinPercent;
    uint32_t detailPercent, colourPercent, autoMask;
} MoonSrNrBridgeConfig;

typedef struct MoonSrNrBridgeFrame {
    uint32_t size;
    void* texture; // borrowed ID3D11Texture2D
    int32_t left, top, right, bottom;
    int64_t captureQpc;
    uint64_t sequence;
    uint32_t reset; // non-zero on a discontinuity, resize, or first frame
    // Live picture adjustments in hundredths; exposure is measured in stops.
    uint32_t pictureEnabled;
    int32_t exposure, contrast, midtones, shadows, highlights;
    int32_t saturation, vibrance, temperature, tint, sharpen;
    // Optional original-capture guide, already resized to the processing image dimensions.
    // Optical flow and scene detection use this instead of the transformed processing image.
    void* flowGuide;
} MoonSrNrBridgeFrame;

typedef struct MoonSrNrBridgeOutput {
    uint32_t size;
    // Borrowed ID3D11Texture2D, valid until the next Process call or Destroy. The texture must be on
    // the device supplied at Create and must be shader-resource bindable.
    void* texture;
    int32_t left, top, right, bottom;
} MoonSrNrBridgeOutput;

typedef struct MoonSrNrBridgeApi {
    uint32_t size;
    uint32_t apiVersion;
    MoonSrNrBridgeResult (*Create)(const MoonSrNrBridgeConfig* config, void** instance, const wchar_t** error);
    void (*Destroy)(void* instance);
    MoonSrNrBridgeResult (*Process)(void* instance, const MoonSrNrBridgeFrame* frame,
                                    MoonSrNrBridgeOutput* output, const wchar_t** error);
} MoonSrNrBridgeApi;

// Export exactly this function from MoonSR_NrBridge.dll.
typedef const MoonSrNrBridgeApi* (*MoonSrNrBridgeGetApiFn)(uint32_t requestedApiVersion);

// Optional ABI 4 export, MoonSR_NrBridge_UpdateSettings. Only the settings fields are read.
// Called on the same thread as Process. Resource changes may take effect at the next Process.
typedef MoonSrNrBridgeResult (*MoonSrNrBridgeUpdateSettingsFn)(void* instance, const MoonSrNrBridgeConfig* settings,
                                                             const wchar_t** error);

