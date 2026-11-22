// GPL-3.0-or-later. This is a buildable ABI probe, not an NR implementation.
#include "moonsr_nr_bridge.h"

namespace {
const wchar_t kUnavailable[] = L"This bridge template has no vendor runtime backend. Install a separately built MoonSR_NrBridge.dll.";

MoonSrNrBridgeResult Create(const MoonSrNrBridgeConfig*, void**, const wchar_t** error) {
    if (error) *error = kUnavailable;
    return MOONSR_NR_BRIDGE_UNSUPPORTED;
}
void Destroy(void*) {}
MoonSrNrBridgeResult Process(void*, const MoonSrNrBridgeFrame*, MoonSrNrBridgeOutput*, const wchar_t** error) {
    if (error) *error = kUnavailable;
    return MOONSR_NR_BRIDGE_UNSUPPORTED;
}
const MoonSrNrBridgeApi kApi{sizeof(MoonSrNrBridgeApi), MOONSR_NR_BRIDGE_API_VERSION, Create, Destroy, Process};
}

extern "C" MOONSR_NR_BRIDGE_EXPORT const MoonSrNrBridgeApi* MoonSR_NrBridge_GetApi(uint32_t requestedApiVersion) {
    return requestedApiVersion == MOONSR_NR_BRIDGE_API_VERSION ? &kApi : nullptr;
}
