// User-supplied model store. Header owned by the lead.
//
// Some optional features need a vendor runtime / model file that MoonSR does not ship and never downloads (for
// example NVIDIA's DLSS Neural Rendering model, nvngx_dlssnr.dll). The user adds their own copy through the UI.
// The store validates the file, copies it into MoonSR's own folder and keeps a small catalogue.
//
// Principles:
//  - Nothing is bundled or downloaded. The user is responsible for having the right to use the file they add.
//  - Importing NEVER loads or executes the file: it is inspected as data only (PE headers, version resource,
//    Authenticode signature, hash). The user sees what the file is before any feature may load it.
//  - The original file is not modified or moved; a copy is stored under the store directory.
//
// Threading: all functions are safe to call from the UI thread; they do blocking file IO (hashing a few hundred MB
// can take a second or two), so the UI should show progress or call them off the UI thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace moonsr {

enum class ModelKind : uint32_t {
    Unknown = 0,
    DlssNeuralRendering, // NVIDIA NGX neural rendering model (original file name nvngx_dlssnr.dll)
    DlssSuperResolution, // nvngx_dlss.dll
};

enum class SignatureStatus : uint32_t {
    Unsigned = 0,   // no Authenticode signature
    Valid,          // signature verifies and chains to a trusted root
    Invalid,        // signature present but does not verify (tampered, revoked, untrusted root, ...)
    Unknown,        // verification could not be performed
};

struct ModelInfo {
    std::wstring id;               // stable id: "<kind>-<first 16 hex chars of sha256>"
    ModelKind kind = ModelKind::Unknown;
    std::wstring displayName;      // e.g. "DLSS Neural Rendering 310.9.16.0"
    std::wstring storedPath;       // full path of the copy inside the store
    std::wstring sourcePath;       // where it was imported from (informational)
    std::wstring originalFileName; // from the version resource
    std::wstring productName;
    std::wstring companyName;
    std::wstring fileVersion;      // "310.9.16.0"
    uint64_t fileSize = 0;
    std::wstring sha256;           // lowercase hex
    SignatureStatus signature = SignatureStatus::Unknown;
    std::wstring signer;           // subject name of the signing certificate, empty if unsigned
    bool is64Bit = false;
    std::wstring importedAt;       // local time, ISO 8601
};

enum class ImportResult : uint32_t {
    Ok = 0,
    AlreadyInstalled, // same sha256 is already in the store (`info` describes the existing entry)
    FileNotFound,
    NotAModel,        // not a 64-bit Windows DLL
    CopyFailed,
    StoreError,       // catalogue could not be written
};

class ModelStore {
public:
    // `directory` e.g. %LOCALAPPDATA%\MoonSR\models (created on demand).
    explicit ModelStore(std::wstring directory);

    // Inspects a file WITHOUT importing or loading it. Fills everything except id/storedPath/importedAt.
    // Returns false if the file cannot be read or is not a PE image (error text in `error`).
    static bool Inspect(const std::wstring& path, ModelInfo& info, std::wstring& error);

    // Validates and copies the file into the store. An unsigned or invalidly signed file IS accepted (the UI
    // must warn and ask first); a file that is not a 64-bit DLL is rejected.
    ImportResult Import(const std::wstring& path, ModelInfo& info, std::wstring& error);

    // Catalogue, newest first. Entries whose stored file has gone missing are dropped.
    std::vector<ModelInfo> List() const;

    // Re-hashes the stored copy and compares with the catalogue (detects a file modified after import).
    bool Verify(const std::wstring& id, std::wstring& error) const;

    // Deletes the stored copy and its catalogue entry. Returns false if the id is unknown.
    bool Remove(const std::wstring& id, std::wstring& error);

    const std::wstring& Directory() const { return directory_; }

    static const wchar_t* KindName(ModelKind kind);

private:
    std::wstring directory_;
};

} // namespace moonsr
