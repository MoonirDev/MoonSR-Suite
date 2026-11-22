// Neural rendering setup helpers for the Neural rendering page: which GPUs can run neural rendering, and importing the
// user's own AMDNR / DLSS5_INTEL runtime files into the folders the bridge loads them from (amd-nr / intel-nr beside
// MoonSR.exe, see Engine::NeuralRuntimeDirectory).
//
// MoonSR never downloads, bundles or converts any runtime, model or weights; these functions only copy files the user
// already has. Archives are untrusted: the central directory is checked first (no absolute paths, no "..", no drive or
// stream names, at most kMaxEntries entries and kMaxArchiveBytes uncompressed), only the expected files are extracted
// with Windows' own tar.exe into a fresh temporary folder, nothing from an archive is ever run, and the destination is
// replaced through a staging folder and a rename, so a failed import leaves the previous install as it was.
//
// Threading: ClassifyGpu and the *Status functions are cheap and may run on any thread. RunImport and ProbeLoad do
// blocking file IO and start processes; call them on a worker thread, never on the UI thread.
#pragma once

#include "core/types.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace moonsr::app::nr {

// ------------------------------------------------------------------------------------------------ GPU support
enum class Support { Supported, Unknown, NotSupported };

struct GpuSupport {
    Support support = Support::Unknown;
    std::wstring family;  // e.g. "RDNA 3 (Navi 31)", "GeForce RTX", "Arc A/B-series"
    std::wstring reason;  // plain-language reason when not Supported (empty otherwise)
};

// The single place that decides which GPUs can run neural rendering. NVIDIA: GeForce / professional RTX. AMD: RDNA 3 and
// RDNA 4 (Radeon RX 7000 / RX 9000) and RDNA 3-class APUs, by PCI device ID (ClassifyAmd). Intel: Arc A- and B-series.
GpuSupport ClassifyGpu(const AdapterInfo& adapter);
GpuSupport ClassifyAmd(uint32_t deviceId);

// ------------------------------------------------------------------------------------------------ install status
struct IntelFiles {
    bool chain = false, shaders = false, weights = false;  // d5c_chain.dll, gemm.spv, dlssnr-logical.safetensors
};
IntelFiles IntelStatus(const std::wstring& folder);
bool AmdFilesPresent(const std::wstring& folder);  // LmxxfNrRuntime.dll and LmxxfNrRuntime.pak
// Reads d5c_chain.dll's headers and export table (nothing is loaded): a 64-bit DLL with the dlss5_* functions the
// bridge calls. Reads a few kilobytes; cache the result by file size and time when calling it often.
bool CheckIntelChain(const std::wstring& dll, std::wstring& error);

// ------------------------------------------------------------------------------------------------ import
enum class ImportKind {
    AmdArchive,    // paths[0]: AMDNR release zip
    AmdFiles,      // paths: LmxxfNrRuntime.dll, LmxxfNrRuntime.pak and optional runtime DLLs
    IntelArchive,  // paths[0]: zip with d5c_chain.dll and its .spv shaders (weights optional)
    IntelFolder,   // paths[0]: folder with the same files
    IntelWeights,  // paths[0]: the user's dlssnr-logical.safetensors
};

struct ImportResult {
    bool installed = false;               // files were copied into the destination
    bool validated = false;               // and the checks after copying passed
    std::wstring message;                 // plain-language summary (error, warning or success)
    std::vector<std::wstring> files;      // file names now installed by this import
};

constexpr uint64_t kMaxArchiveBytes = 2ull << 30;  // total uncompressed size accepted from one archive or folder
constexpr size_t kMaxEntries = 20000;               // entries accepted in one archive

// Copies the user's files into `destination` (the amd-nr or intel-nr folder). `progress` (optional) receives short
// status lines such as "Copying LmxxfNrRuntime.pak" from the worker thread. AMD imports are validated by loading the
// runtime in a separate MoonSR.exe process (ProbeLoad); Intel imports by the export table and the bridge's file check.
ImportResult RunImport(ImportKind kind, const std::vector<std::wstring>& paths, const std::wstring& destination,
                       const std::function<void(const std::wstring&)>& progress);

// Loads `dll` the way the bridge does (LoadLibraryExW with the DLL's folder and the default directories) in a child
// MoonSR.exe started with --nr-probe-load, checks that it exports `exportName`, and never calls into it. Returns true
// when it loads; otherwise `error` explains why. Waits at most 30 s. Worker thread only.
bool ProbeLoad(const std::wstring& dll, const std::string& exportName, std::wstring& error);

// The child side of ProbeLoad (main.cpp). Returns the process exit code: 0 loaded and export found, the Win32 error of
// LoadLibraryExW, or kProbeNoExport.
constexpr int kProbeNoExport = 0x20000001;
int RunProbeChild(const std::wstring& dll, const std::wstring& exportName);

}  // namespace moonsr::app::nr
