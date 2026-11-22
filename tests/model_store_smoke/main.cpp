// moonsr_model_store_smoke: console smoke test for src/models (ModelStore).
//
// Uses stand-in DLLs (never a real nvngx_dlssnr.dll) and a fresh store directory under benchmarks/output/model_store_smoke/.
// Checks: Inspect / Import / AlreadyInstalled / List / Verify / tamper detection / Remove, rejection of non-DLLs, missing
// files and 32-bit DLLs, kind detection by file name, a corrupt catalogue, and that importing never loads the file
// into this process. Prints PASS/FAIL per check; exit code 0 only when every check passed.
#include "models/model_store.h"

#include <windows.h>

#include <psapi.h>

#include <cstdio>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace moonsr;
namespace fs = std::filesystem;

namespace {

int g_fail = 0, g_pass = 0;

void Check(bool ok, const std::wstring& what) {
    wprintf(L"  [%s] %s\n", ok ? L"PASS" : L"FAIL", what.c_str());
    (ok ? g_pass : g_fail)++;
}

const wchar_t* SigName(SignatureStatus s) {
    switch (s) {
    case SignatureStatus::Unsigned: return L"Unsigned";
    case SignatureStatus::Valid: return L"Valid";
    case SignatureStatus::Invalid: return L"Invalid";
    default: return L"Unknown";
    }
}

const wchar_t* ResultName(ImportResult r) {
    switch (r) {
    case ImportResult::Ok: return L"Ok";
    case ImportResult::AlreadyInstalled: return L"AlreadyInstalled";
    case ImportResult::FileNotFound: return L"FileNotFound";
    case ImportResult::NotAModel: return L"NotAModel";
    case ImportResult::CopyFailed: return L"CopyFailed";
    default: return L"StoreError";
    }
}

void Print(const ModelInfo& m) {
    wprintf(L"    name=%s kind=%s version=%s 64bit=%d size=%llu\n", m.displayName.c_str(), ModelStore::KindName(m.kind),
            m.fileVersion.c_str(), m.is64Bit ? 1 : 0, static_cast<unsigned long long>(m.fileSize));
    wprintf(L"    original='%s' product='%s' company='%s'\n", m.originalFileName.c_str(), m.productName.c_str(), m.companyName.c_str());
    wprintf(L"    sha256=%s\n    signature=%s signer='%s'\n", m.sha256.c_str(), SigName(m.signature), m.signer.c_str());
}

bool ModuleLoadedUnder(const std::wstring& dir) {
    HMODULE mods[2048];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return true;  // cannot prove it: fail
    const std::wstring prefix = fs::weakly_canonical(dir).wstring();
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 2048; ++i) {
        wchar_t p[MAX_PATH * 2] = {};
        if (!GetModuleFileNameExW(GetCurrentProcess(), mods[i], p, MAX_PATH * 2)) continue;
        std::wstring s = p;
        if (s.size() >= 4 && s.compare(0, 4, L"\\\\?\\") == 0) s = s.substr(4);
        if (_wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0) return true;
    }
    return false;
}

bool FilesEqual(const std::wstring& a, const std::wstring& b) {
    std::ifstream fa(fs::path(a), std::ios::binary), fb(fs::path(b), std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> ba(1 << 16), bb(1 << 16);
    for (;;) {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        if (fa.gcount() != fb.gcount()) return false;
        if (fa.gcount() == 0) return true;
        if (memcmp(ba.data(), bb.data(), static_cast<size_t>(fa.gcount())) != 0) return false;
    }
}

// Finds a 64-bit DLL with an EMBEDDED Authenticode signature that verifies, to prove signer extraction.
bool FindEmbeddedSigned(std::wstring& found) {
    std::vector<fs::path> roots = {L"C:\\Windows\\System32\\DriverStore\\FileRepository", L"C:\\Program Files\\Git",
                                   L"C:\\Program Files\\Microsoft Visual Studio", L"C:\\Program Files (x86)\\Microsoft Visual Studio",
                                   L"C:\\Windows\\System32"};
    int inspected = 0;
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            const std::wstring ext = it->path().extension().wstring();
            if (_wcsicmp(ext.c_str(), L".dll") != 0) continue;
            if (it->file_size(e2) > (24u << 20)) continue;
            if (++inspected > 600) return false;
            ModelInfo m;
            std::wstring err;
            if (ModelStore::Inspect(it->path().wstring(), m, err) && m.is64Bit && m.signature == SignatureStatus::Valid &&
                !m.signer.empty()) {
                found = it->path().wstring();
                return true;
            }
        }
    }
    return false;
}

void WriteText(const std::wstring& path, const std::string& text) {
    std::ofstream f(fs::path(path), std::ios::binary);
    f << text;
}

}  // namespace

bool Exists(const std::wstring& p) {
    std::error_code ec;
    return fs::exists(fs::path(p), ec);
}

// Runs the full lifecycle for one stand-in: import, duplicate, list, verify, tamper, remove.
void Lifecycle(ModelStore& store, const std::wstring& source, const wchar_t* label) {
    wprintf(L"\n== Lifecycle: %s\n", label);
    std::wstring err;
    ModelInfo src;
    Check(ModelStore::Inspect(source, src, err), L"Inspect source");
    ModelInfo imp;
    ImportResult r = store.Import(source, imp, err);
    Check(r == ImportResult::Ok, std::wstring(L"Import -> ") + ResultName(r) + L" " + err);
    if (r != ImportResult::Ok) return;
    Check(Exists(imp.storedPath), L"stored copy exists: " + imp.storedPath);
    ModelInfo st;
    Check(ModelStore::Inspect(imp.storedPath, st, err) && st.sha256 == src.sha256, L"stored copy SHA-256 equals the source's");
    Check(FilesEqual(source, imp.storedPath), L"stored copy is byte-identical to the source");
    Check(imp.sha256 == src.sha256 && imp.signature == src.signature && imp.signer == src.signer,
          L"import result carries the same hash / signature / signer");

    ModelInfo again;
    r = store.Import(source, again, err);
    Check(r == ImportResult::AlreadyInstalled && again.id == imp.id, std::wstring(L"second Import -> ") + ResultName(r));

    auto list = store.List();
    Check(list.size() == 1 && list[0].id == imp.id, L"List returns exactly the imported model");
    const bool v1 = store.Verify(imp.id, err);
    Check(v1, L"Verify passes on the untouched copy");
    const bool v2 = store.Verify(L"does-not-exist", err);
    Check(!v2, L"Verify of an unknown id fails: " + err);

    {  // flip one byte
        HANDLE h = CreateFileW(imp.storedPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool flipped = false;
        if (h != INVALID_HANDLE_VALUE) {
            unsigned char b = 0;
            DWORD n = 0;
            SetFilePointer(h, 1000, nullptr, FILE_BEGIN);
            if (ReadFile(h, &b, 1, &n, nullptr) && n == 1) {
                b ^= 0xFF;
                SetFilePointer(h, 1000, nullptr, FILE_BEGIN);
                flipped = WriteFile(h, &b, 1, &n, nullptr) && n == 1;
            }
            CloseHandle(h);
        }
        Check(flipped, L"flipped one byte in the stored copy");
    }
    const bool v3 = store.Verify(imp.id, err);
    Check(!v3, L"Verify reports a mismatch after tampering (expected negative result): " + err);

    Check(store.Remove(imp.id, err), L"Remove succeeds");
    Check(!Exists(imp.storedPath) && !Exists(fs::path(imp.storedPath).parent_path().wstring()), L"stored copy and its folder are gone");
    Check(store.List().empty(), L"List is empty after Remove");
    Check(!store.Remove(imp.id, err), L"Remove of an unknown id returns false");
}

int wmain() {
    const fs::path root = fs::path(MOONSR_SOURCE_DIR) / L"benchmarks" / L"output" / L"model_store_smoke";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const std::wstring storeDir = (root / L"store").wstring();
    const std::wstring inputs = (root / L"inputs").wstring();
    fs::create_directories(inputs, ec);
    wprintf(L"Store directory: %s\n", storeDir.c_str());

    ModelStore store(storeDir);
    std::wstring err;

    wprintf(L"\n== Inspect: C:\\Windows\\System32\\d3d11.dll (64-bit system DLL)\n");
    const std::wstring sys = L"C:\\Windows\\System32\\d3d11.dll";
    ModelInfo info;
    Check(ModelStore::Inspect(sys, info, err), L"Inspect succeeds " + err);
    Print(info);
    Check(info.is64Bit, L"is64Bit");
    Check(!info.fileVersion.empty(), L"file version present: " + info.fileVersion);
    Check(info.signature == SignatureStatus::Valid || info.signature == SignatureStatus::Unsigned,
          std::wstring(L"signature is Valid or Unsigned (observed: ") + SigName(info.signature) + L")");
    Check(info.sha256.size() == 64, L"sha256 has 64 hex chars");
    Check(info.kind == ModelKind::Unknown && info.displayName.find(L"Unknown model (d3d11.dll)") == 0, L"kind Unknown, display name: " + info.displayName);
    const SignatureStatus sysSig = info.signature;

    Lifecycle(store, sys, L"d3d11.dll");

    // ---------------------------------------------------------------- embedded Authenticode signature
    wprintf(L"\n== Embedded-signature stand-in\n");
    std::wstring signedPath;
    if (FindEmbeddedSigned(signedPath)) {
        wprintf(L"  found: %s\n", signedPath.c_str());
        ModelInfo s;
        Check(ModelStore::Inspect(signedPath, s, err), L"Inspect");
        Print(s);
        Check(s.signature == SignatureStatus::Valid && !s.signer.empty(), L"signature Valid with signer '" + s.signer + L"'");
        Lifecycle(store, signedPath, L"embedded-signed DLL");
    } else {
        Check(false, L"no embedded-signed 64-bit DLL found on this machine (system DLL status was " + std::wstring(SigName(sysSig)) + L")");
    }

    // ---------------------------------------------------------------- unsigned / tampered stand-ins
    // Both are derived from d3d11.dll: "unsigned" has its certificate table removed, "tampered" has one code byte flipped
    // (the embedded signature stays but no longer matches). The signature check must classify them differently.
    wprintf(L"\n== Unsigned and tampered stand-ins\n");
    {
        std::ifstream in(fs::path(sys), std::ios::binary);
        std::vector<unsigned char> img((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        bool built = false;
        std::vector<unsigned char> stripped = img;
        if (img.size() > 0x1000 && img[0] == 'M') {
            const uint32_t pe = *reinterpret_cast<const uint32_t*>(&img[0x3c]);
            const size_t opt = pe + 24;
            const uint16_t magic = *reinterpret_cast<const uint16_t*>(&img[opt]);
            const size_t dd = opt + (magic == 0x20b ? 112 : 96) + 4 * 8;  // IMAGE_DIRECTORY_ENTRY_SECURITY
            uint32_t off = *reinterpret_cast<const uint32_t*>(&img[dd]);
            if (off != 0 && off < img.size()) {
                memset(&stripped[dd], 0, 8);
                stripped.resize(off);
                built = true;
            }
        }
        Check(built, L"built the unsigned stand-in (certificate table removed)");
        if (built) {
            fs::create_directories(inputs + L"\\uns", ec);
            fs::create_directories(inputs + L"\\bad", ec);
            const std::wstring unsPath = inputs + L"\\uns\\unsigned_standin.dll";
            const std::wstring badPath = inputs + L"\\bad\\tampered_standin.dll";
            { std::ofstream o(fs::path(unsPath), std::ios::binary); o.write(reinterpret_cast<const char*>(stripped.data()), static_cast<std::streamsize>(stripped.size())); }
            std::vector<unsigned char> bad = img;
            bad[0x1000] ^= 0xFF;
            { std::ofstream o(fs::path(badPath), std::ios::binary); o.write(reinterpret_cast<const char*>(bad.data()), static_cast<std::streamsize>(bad.size())); }
            ModelInfo u, b;
            Check(ModelStore::Inspect(unsPath, u, err), L"Inspect unsigned stand-in");
            Check(u.signature == SignatureStatus::Unsigned && u.signer.empty(), std::wstring(L"unsigned stand-in -> Unsigned (observed: ") + SigName(u.signature) + L", signer '" + u.signer + L"')");
            Check(ModelStore::Inspect(badPath, b, err), L"Inspect tampered stand-in");
            Check(b.signature == SignatureStatus::Invalid, std::wstring(L"tampered stand-in -> Invalid (observed: ") + SigName(b.signature) + L")");
        }
    }

    // ---------------------------------------------------------------- rejections
    wprintf(L"\n== Rejections\n");
    const std::wstring fakeDll = inputs + L"\\notes.dll";
    WriteText(fakeDll, "this is plain text, not a PE file\n");
    ModelInfo x;
    ImportResult r = store.Import(fakeDll, x, err);
    Check(r == ImportResult::NotAModel, std::wstring(L"text file renamed .dll -> ") + ResultName(r) + L" (" + err + L")");
    r = store.Import(inputs + L"\\missing.dll", x, err);
    Check(r == ImportResult::FileNotFound, std::wstring(L"missing path -> ") + ResultName(r));
    r = store.Import(L"C:\\Windows\\SysWOW64\\d3d11.dll", x, err);
    Check(r == ImportResult::NotAModel, std::wstring(L"32-bit DLL (SysWOW64) -> ") + ResultName(r) + L" (" + err + L")");
    r = store.Import(L"C:\\Windows\\System32\\notepad.exe", x, err);
    Check(r == ImportResult::NotAModel, std::wstring(L"64-bit EXE -> ") + ResultName(r) + L" (" + err + L")");
    Check(store.List().empty(), L"nothing was stored by the rejected files");

    // ---------------------------------------------------------------- kind by file name
    wprintf(L"\n== A copy of d3d11.dll named nvngx_dlssnr.dll\n");
    fs::create_directories(inputs + L"\\nr", ec);
    const std::wstring renamed = inputs + L"\\nr\\nvngx_dlssnr.dll";
    fs::copy_file(sys, renamed, fs::copy_options::overwrite_existing, ec);
    Check(!ec, L"created the renamed copy");
    ModelInfo ri;
    r = store.Import(renamed, ri, err);
    Check(r == ImportResult::Ok, std::wstring(L"Import -> ") + ResultName(r));
    Print(ri);
    Check(ri.originalFileName != L"nvngx_dlssnr.dll", L"version resource OriginalFilename is '" + ri.originalFileName + L"' (rule 1 does not fire)");
    Check(ri.kind == ModelKind::DlssNeuralRendering, L"kind DlssNeuralRendering: decided by rule 2 (on-disk file name nvngx_dlssnr.dll)");
    Check(ri.displayName.find(L"DLSS Neural Rendering ") == 0, L"display name: " + ri.displayName);
    Check(ri.id.find(L"dlssneuralrendering-") == 0, L"id: " + ri.id);
    wprintf(L"  catalogue after import:\n");
    {
        std::ifstream f(fs::path(storeDir) / L"catalogue.txt", std::ios::binary);
        std::string line;
        while (std::getline(f, line)) printf("    | %s\n", line.c_str());
    }
    {
        ModelStore again(storeDir);
        Check(again.List().size() == 1, L"a new ModelStore instance reads the persisted catalogue");
    }

    // ---------------------------------------------------------------- never loads
    wprintf(L"\n== Never loads\n");
    HMODULE byName = GetModuleHandleW(L"nvngx_dlssnr.dll");
    Check(byName == nullptr, L"GetModuleHandleW(\"nvngx_dlssnr.dll\") is null");
    Check(!ModuleLoadedUnder(storeDir), L"no module of this process lies under the store directory");
    Check(store.Remove(ri.id, err), L"Remove the renamed model");

    // ---------------------------------------------------------------- corrupt catalogue
    wprintf(L"\n== Corrupt catalogue\n");
    const std::wstring corruptDir = (root / L"corrupt_store").wstring();
    fs::create_directories(corruptDir, ec);
    ModelStore bad(corruptDir);
    WriteText(corruptDir + L"\\catalogue.txt", std::string("\x01\x02garbage\n[model]\nid=\xff\xfe\nsha256=zz\n\n\n===\n"));
    Check(bad.List().empty(), L"corrupt catalogue (bad header) -> empty list, no crash");
    WriteText(corruptDir + L"\\catalogue.txt",
              "MOONSR-MODEL-CATALOGUE 1\n\n[model]\nid=..\\..\\evil\nfile=x.dll\nsha256=" + std::string(64, 'a') +
                  "\n[model]\nid=\nsha256=\n[model\nkey\n");
    Check(bad.List().empty(), L"catalogue with unsafe / incomplete entries -> empty list");
    ModelInfo bi;
    r = bad.Import(sys, bi, err);
    Check(r == ImportResult::Ok && bad.List().size() == 1, std::wstring(L"store recovers: Import after a corrupt catalogue -> ") + ResultName(r));
    Check(!ModuleLoadedUnder(corruptDir), L"no module loaded from the second store either");

    wprintf(L"\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
