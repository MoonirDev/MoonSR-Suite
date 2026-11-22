// User-supplied model store: inspection and import of a model file as DATA.
//
// Nothing here loads or executes the file: no LoadLibrary, no process creation. The PE headers are parsed from
// buffered reads with explicit bounds checks; the version resource is read with GetFileVersionInfoExW (resource
// parsing only); the signature is checked with WinVerifyTrust / CryptQueryObject (parsing and chain validation only).
//
// Threading: every function may be called from any thread. The catalogue file is read-modify-written under one
// process-wide mutex; hashing and copying happen outside it.
#include "models/model_store.h"

#include "core/log.h"

#include <windows.h>

#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace moonsr {
namespace {

std::mutex g_catalogueMutex;

constexpr const wchar_t* kCatalogueName = L"catalogue.txt";
constexpr const char* kCatalogueHeader = "MOONSR-MODEL-CATALOGUE 1";

// ------------------------------------------------------------------------------------------------ small utilities

struct FileHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    FileHandle() = default;
    explicit FileHandle(HANDLE handle) : h(handle) {}
    FileHandle(FileHandle&& o) noexcept : h(o.h) { o.h = INVALID_HANDLE_VALUE; }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() { Close(); }
    void Close() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
};

FileHandle OpenForRead(const std::wstring& path, DWORD* lastError = nullptr) {
    FileHandle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!f.ok() && lastError) *lastError = GetLastError();
    return f;
}

std::wstring Win32Message(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s = (n && buf) ? std::wstring(buf, n) : std::wstring();
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    return s + L" (error " + std::to_wstring(code) + L")";
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool EqualNoCase(const std::wstring& a, const wchar_t* b) { return Lower(a) == Lower(b); }

std::wstring FileNameOf(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring NowIso() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t buf[40];
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

void CreateDirectoriesRecursive(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    }
}

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// An id is used as a folder name: it must not be able to escape the store directory.
bool IdIsSafe(const std::wstring& id) {
    if (id.empty() || id.size() > 100) return false;
    for (wchar_t c : id)
        if (!(std::iswalnum(c) || c == L'-' || c == L'_')) return false;
    return true;
}

bool NameIsSafe(const std::wstring& n) {
    if (n.empty() || n == L"." || n == L"..") return false;
    return n.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos;
}

// ------------------------------------------------------------------------------------------------ SHA-256 (BCrypt)

struct HashAlg {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~HashAlg() {
        if (h) BCryptCloseAlgorithmProvider(h, 0);
    }
};
struct HashObj {
    BCRYPT_HASH_HANDLE h = nullptr;
    ~HashObj() {
        if (h) BCryptDestroyHash(h);
    }
};

bool Sha256File(const std::wstring& path, std::wstring& hex, uint64_t* size, std::wstring& error) {
    DWORD le = 0;
    FileHandle f = OpenForRead(path, &le);
    if (!f.ok()) {
        error = L"Cannot open the file: " + Win32Message(le);
        return false;
    }
    HashAlg alg;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (st < 0) {
        error = L"SHA-256 is not available (BCrypt status " + std::to_wstring(static_cast<unsigned>(st)) + L")";
        return false;
    }
    DWORD objLen = 0, got = 0;
    st = BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &got, 0);
    if (st < 0) {
        error = L"BCryptGetProperty failed";
        return false;
    }
    std::vector<UCHAR> obj(objLen);
    HashObj hash;
    st = BCryptCreateHash(alg.h, &hash.h, obj.data(), objLen, nullptr, 0, 0);
    if (st < 0) {
        error = L"BCryptCreateHash failed";
        return false;
    }
    std::vector<UCHAR> buf(1u << 20);
    uint64_t total = 0;
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(f.h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
            error = L"Read error: " + Win32Message(GetLastError());
            return false;
        }
        if (n == 0) break;
        total += n;
        st = BCryptHashData(hash.h, buf.data(), n, 0);
        if (st < 0) {
            error = L"BCryptHashData failed";
            return false;
        }
    }
    UCHAR digest[32] = {};
    st = BCryptFinishHash(hash.h, digest, sizeof(digest), 0);
    if (st < 0) {
        error = L"BCryptFinishHash failed";
        return false;
    }
    static const wchar_t* kHex = L"0123456789abcdef";
    hex.clear();
    for (UCHAR b : digest) {
        hex.push_back(kHex[b >> 4]);
        hex.push_back(kHex[b & 15]);
    }
    if (size) *size = total;
    return true;
}

// ------------------------------------------------------------------------------------------------ PE headers

bool ReadAt(HANDLE h, uint64_t offset, void* dst, DWORD len) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD n = 0;
    if (!ReadFile(h, dst, len, &n, &ov)) return false;
    return n == len;
}

struct PeFacts {
    bool is64Bit = false;
    bool isDll = false;
    WORD machine = 0;
};

// Parses DOS + NT headers with bounds checks against the real file size.
bool ParsePe(const std::wstring& path, uint64_t fileSize, PeFacts& out, std::wstring& error) {
    DWORD le = 0;
    FileHandle f = OpenForRead(path, &le);
    if (!f.ok()) {
        error = L"Cannot open the file: " + Win32Message(le);
        return false;
    }
    IMAGE_DOS_HEADER dos{};
    if (fileSize < sizeof(dos) || !ReadAt(f.h, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        error = L"This is not a Windows program or library (no MZ header).";
        return false;
    }
    const uint64_t nt = static_cast<uint32_t>(dos.e_lfanew);
    // Signature (4) + file header (20) + optional header magic (2) must fit.
    if (nt < sizeof(dos) || nt + 4 + sizeof(IMAGE_FILE_HEADER) + 2 > fileSize) {
        error = L"This is not a valid Windows library (the PE header offset is out of range).";
        return false;
    }
    DWORD sig = 0;
    IMAGE_FILE_HEADER fh{};
    WORD magic = 0;
    if (!ReadAt(f.h, nt, &sig, 4) || sig != IMAGE_NT_SIGNATURE || !ReadAt(f.h, nt + 4, &fh, sizeof(fh))) {
        error = L"This is not a valid Windows library (no PE signature).";
        return false;
    }
    if (fh.SizeOfOptionalHeader < sizeof(WORD) || nt + 4 + sizeof(fh) + fh.SizeOfOptionalHeader > fileSize ||
        !ReadAt(f.h, nt + 4 + sizeof(fh), &magic, sizeof(magic))) {
        error = L"This is not a valid Windows library (optional header missing or truncated).";
        return false;
    }
    if (magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC && magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = L"This is not a valid Windows library (unknown optional header).";
        return false;
    }
    out.machine = fh.Machine;
    out.isDll = (fh.Characteristics & IMAGE_FILE_DLL) != 0;
    const bool machine64 = fh.Machine == IMAGE_FILE_MACHINE_AMD64 || fh.Machine == IMAGE_FILE_MACHINE_ARM64 ||
                           fh.Machine == IMAGE_FILE_MACHINE_IA64;
    out.is64Bit = magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && machine64;
    return true;
}

// ------------------------------------------------------------------------------------------------ version resource

struct VersionFacts {
    std::wstring fileVersion, originalFileName, productName, companyName;
};

void ReadVersion(const std::wstring& path, VersionFacts& out) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &handle);
    if (size == 0 || size > (16u << 20)) return;  // no version resource (or an absurd one)
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), 0, size, data.data())) return;

    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) && ffi && len >= sizeof(VS_FIXEDFILEINFO) &&
        ffi->dwSignature == 0xFEEF04BD) {
        out.fileVersion = std::to_wstring(HIWORD(ffi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(ffi->dwFileVersionMS)) +
                          L"." + std::to_wstring(HIWORD(ffi->dwFileVersionLS)) + L"." +
                          std::to_wstring(LOWORD(ffi->dwFileVersionLS));
    }

    struct LangCp {
        WORD lang, cp;
    };
    std::vector<LangCp> tries;
    LangCp* tr = nullptr;
    UINT trLen = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&tr), &trLen) && tr) {
        for (UINT i = 0; i < trLen / sizeof(LangCp) && i < 8; ++i) tries.push_back(tr[i]);
    }
    tries.push_back({0x0409, 0x04B0});
    tries.push_back({0x0409, 0x04E4});
    tries.push_back({0x0000, 0x04B0});

    auto query = [&](const wchar_t* name) -> std::wstring {
        for (const LangCp& t : tries) {
            wchar_t sub[96];
            swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\%s", t.lang, t.cp, name);
            const wchar_t* val = nullptr;
            UINT vlen = 0;
            if (VerQueryValueW(data.data(), sub, reinterpret_cast<LPVOID*>(const_cast<wchar_t**>(&val)), &vlen) && val &&
                vlen > 0) {
                size_t n = wcsnlen(val, vlen);
                return std::wstring(val, n);
            }
        }
        return {};
    };
    out.originalFileName = query(L"OriginalFilename");
    out.productName = query(L"ProductName");
    out.companyName = query(L"CompanyName");
}

// ------------------------------------------------------------------------------------------------ Authenticode

// Subject name of the (first) embedded signer, whether or not the signature validates.
std::wstring EmbeddedSignerName(const std::wstring& path) {
    HCERTSTORE store = nullptr;
    HCRYPTMSG msg = nullptr;
    DWORD enc = 0, content = 0, format = 0;
    std::wstring name;
    if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                         CERT_QUERY_FORMAT_FLAG_BINARY, 0, &enc, &content, &format, &store, &msg, nullptr)) {
        DWORD cb = 0;
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &cb) && cb > 0) {
            std::vector<BYTE> buf(cb);
            if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, buf.data(), &cb)) {
                const auto* si = reinterpret_cast<const CMSG_SIGNER_INFO*>(buf.data());
                CERT_INFO ci{};
                ci.Issuer = si->Issuer;
                ci.SerialNumber = si->SerialNumber;
                PCCERT_CONTEXT cert =
                    CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci, nullptr);
                if (cert) {
                    const DWORD n = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
                    if (n > 1) {
                        std::wstring s(n, L'\0');
                        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, s.data(), n);
                        s.resize(wcslen(s.c_str()));
                        name = s;
                    }
                    CertFreeCertificateContext(cert);
                }
            }
        }
    }
    if (msg) CryptMsgClose(msg);
    if (store) CertCloseStore(store, 0);
    return name;
}

SignatureStatus CheckSignature(const std::wstring& path, std::wstring& signer) {
    signer.clear();
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = path.c_str();

    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;  // no network round trips; the signature chain itself is still verified
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    SetLastError(0);
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    const DWORD lastError = GetLastError();
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);

    signer = EmbeddedSignerName(path);

    if (status == ERROR_SUCCESS) return SignatureStatus::Valid;
    const LONG s = status;
    if (s == TRUST_E_NOSIGNATURE) {
        // Same classification as Microsoft's WinVerifyTrust sample.
        if (lastError == TRUST_E_NOSIGNATURE || lastError == TRUST_E_SUBJECT_FORM_UNKNOWN || lastError == TRUST_E_PROVIDER_UNKNOWN ||
            lastError == ERROR_SUCCESS)
            return SignatureStatus::Unsigned;
        return SignatureStatus::Invalid;
    }
    if (s == TRUST_E_PROVIDER_UNKNOWN || s == TRUST_E_SUBJECT_FORM_UNKNOWN) return SignatureStatus::Unsigned;
    if (s == static_cast<LONG>(ERROR_FILE_NOT_FOUND) || s == static_cast<LONG>(ERROR_ACCESS_DENIED) ||
        s == static_cast<LONG>(TRUST_E_SYSTEM_ERROR))
        return SignatureStatus::Unknown;
    return SignatureStatus::Invalid;  // TRUST_E_SUBJECT_NOT_TRUSTED, TRUST_E_BAD_DIGEST, CERT_E_UNTRUSTEDROOT, ...
}

// ------------------------------------------------------------------------------------------------ kind / naming

ModelKind KindFromName(const std::wstring& name) {
    if (name.empty()) return ModelKind::Unknown;
    if (EqualNoCase(name, L"nvngx_dlssnr.dll")) return ModelKind::DlssNeuralRendering;
    if (EqualNoCase(name, L"nvngx_dlss.dll")) return ModelKind::DlssSuperResolution;
    return ModelKind::Unknown;
}

std::wstring DisplayNameFor(const ModelInfo& m, const std::wstring& diskName) {
    std::wstring n;
    switch (m.kind) {
    case ModelKind::DlssNeuralRendering: n = L"DLSS Neural Rendering"; break;
    case ModelKind::DlssSuperResolution: n = L"DLSS Super Resolution"; break;
    default: n = L"Unknown model (" + diskName + L")"; break;
    }
    if (!m.fileVersion.empty()) n += L" " + m.fileVersion;
    return n;
}

std::wstring KindToken(ModelKind k) { return Lower(ModelStore::KindName(k)); }

// ------------------------------------------------------------------------------------------------ catalogue

std::wstring Escape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\n') o += L"\\n";
        else if (c == L'\r') o += L"\\r";
        else o.push_back(c);
    }
    return o;
}

std::wstring Unescape(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            const wchar_t n = s[++i];
            o.push_back(n == L'n' ? L'\n' : n == L'r' ? L'\r' : n);
        } else {
            o.push_back(s[i]);
        }
    }
    return o;
}

struct Entry {
    ModelInfo info;
    std::wstring storedName;  // file name inside <directory>\<id>\ .
};

std::wstring StoredPath(const std::wstring& dir, const Entry& e) { return dir + L"\\" + e.info.id + L"\\" + e.storedName; }

std::wstring CataloguePath(const std::wstring& dir) { return dir + L"\\" + kCatalogueName; }

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    FileHandle f = OpenForRead(path);
    if (!f.ok()) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f.h, &sz) || sz.QuadPart > (64ll << 20)) return false;
    out.resize(static_cast<size_t>(sz.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD n = 0;
        if (!ReadFile(f.h, out.data() + done, static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 20)), &n, nullptr) || n == 0)
            return false;
        done += n;
    }
    return true;
}

// Tolerant parser: a missing or corrupt file yields an empty list (and a log line), never a crash.
std::vector<Entry> LoadCatalogue(const std::wstring& dir) {
    std::vector<Entry> list;
    try {
        std::string raw;
        const std::wstring path = CataloguePath(dir);
        if (!FileExists(path)) return list;
        if (!ReadWholeFile(path, raw)) {
            MSR_LOG_WARN(L"Model catalogue could not be read; starting empty ({})", path);
            return list;
        }
        if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3);  // UTF-8 BOM
        if (raw.compare(0, std::strlen(kCatalogueHeader), kCatalogueHeader) != 0) {
            MSR_LOG_WARN(L"Model catalogue has no valid header; starting empty ({})", path);
            return list;
        }
        Entry cur;
        bool inBlock = false;
        auto flush = [&] {
            if (inBlock && IdIsSafe(cur.info.id) && NameIsSafe(cur.storedName) && cur.info.sha256.size() == 64) {
                cur.info.storedPath = StoredPath(dir, cur);
                list.push_back(cur);
            } else if (inBlock) {
                MSR_LOG_WARN(L"Model catalogue: skipped an incomplete entry");
            }
            cur = Entry{};
            inBlock = false;
        };
        size_t pos = 0;
        while (pos <= raw.size()) {
            size_t nl = raw.find('\n', pos);
            if (nl == std::string::npos) nl = raw.size();
            std::string line = raw.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line == "[model]") {
                flush();
                inBlock = true;
                continue;
            }
            if (!inBlock) continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            const std::wstring val = Unescape(FromUtf8(line.substr(eq + 1)));
            ModelInfo& m = cur.info;
            if (key == "id") m.id = val;
            else if (key == "kind") m.kind = val == L"dlssneuralrendering" ? ModelKind::DlssNeuralRendering
                                              : val == L"dlsssuperresolution" ? ModelKind::DlssSuperResolution
                                                                              : ModelKind::Unknown;
            else if (key == "displayName") m.displayName = val;
            else if (key == "file") cur.storedName = val;
            else if (key == "source") m.sourcePath = val;
            else if (key == "originalFileName") m.originalFileName = val;
            else if (key == "product") m.productName = val;
            else if (key == "company") m.companyName = val;
            else if (key == "version") m.fileVersion = val;
            else if (key == "size") m.fileSize = _wcstoui64(val.c_str(), nullptr, 10);
            else if (key == "sha256") m.sha256 = Lower(val);
            else if (key == "signature") {
                const unsigned v = static_cast<unsigned>(_wtoi(val.c_str()));
                m.signature = v <= static_cast<unsigned>(SignatureStatus::Unknown) ? static_cast<SignatureStatus>(v) : SignatureStatus::Unknown;
            } else if (key == "signer") m.signer = val;
            else if (key == "is64Bit") m.is64Bit = val == L"1";
            else if (key == "importedAt") m.importedAt = val;
        }
        flush();
    } catch (const std::exception& e) {
        MSR_LOG_WARN(L"Model catalogue parse failed ({}); starting empty", FromUtf8(e.what()));
        list.clear();
    }
    return list;
}

bool SaveCatalogue(const std::wstring& dir, const std::vector<Entry>& list, std::wstring& error) {
    try {
        std::string out = std::string(kCatalogueHeader) + "\n";
        auto put = [&](const char* key, const std::wstring& v) { out += std::string(key) + "=" + ToUtf8(Escape(v)) + "\n"; };
        for (const Entry& e : list) {
            const ModelInfo& m = e.info;
            out += "\n[model]\n";
            put("id", m.id);
            put("kind", KindToken(m.kind));
            put("displayName", m.displayName);
            put("file", e.storedName);
            put("source", m.sourcePath);
            put("originalFileName", m.originalFileName);
            put("product", m.productName);
            put("company", m.companyName);
            put("version", m.fileVersion);
            put("size", std::to_wstring(m.fileSize));
            put("sha256", m.sha256);
            put("signature", std::to_wstring(static_cast<unsigned>(m.signature)));
            put("signer", m.signer);
            put("is64Bit", m.is64Bit ? L"1" : L"0");
            put("importedAt", m.importedAt);
        }
        CreateDirectoriesRecursive(dir);
        const std::wstring path = CataloguePath(dir);
        const std::wstring tmp = path + L".tmp";
        {
            FileHandle f(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!f.ok()) {
                error = L"Cannot write the model catalogue: " + Win32Message(GetLastError());
                return false;
            }
            size_t done = 0;
            while (done < out.size()) {
                DWORD n = 0;
                if (!WriteFile(f.h, out.data() + done, static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 20)), &n, nullptr)) {
                    error = L"Cannot write the model catalogue: " + Win32Message(GetLastError());
                    return false;
                }
                done += n;
            }
            FlushFileBuffers(f.h);
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = L"Cannot replace the model catalogue: " + Win32Message(GetLastError());
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = L"Cannot write the model catalogue: " + FromUtf8(e.what());
        return false;
    }
}

}  // namespace

// ------------------------------------------------------------------------------------------------ ModelStore

ModelStore::ModelStore(std::wstring directory) : directory_(std::move(directory)) {
    while (directory_.size() > 3 && (directory_.back() == L'\\' || directory_.back() == L'/')) directory_.pop_back();
}

const wchar_t* ModelStore::KindName(ModelKind kind) {
    switch (kind) {
    case ModelKind::DlssNeuralRendering: return L"DlssNeuralRendering";
    case ModelKind::DlssSuperResolution: return L"DlssSuperResolution";
    default: return L"Unknown";
    }
}

bool ModelStore::Inspect(const std::wstring& path, ModelInfo& info, std::wstring& error) {
    try {
        info = ModelInfo{};
        error.clear();
        const DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            error = L"The file was not found: " + Win32Message(GetLastError());
            return false;
        }
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            error = L"That is a folder, not a file.";
            return false;
        }

        uint64_t size = 0;
        std::wstring sha;
        if (!Sha256File(path, sha, &size, error)) return false;

        PeFacts pe;
        if (!ParsePe(path, size, pe, error)) return false;
        if (!pe.isDll) {
            error = L"This is a program, not a library (.dll).";
            return false;
        }

        VersionFacts ver;
        ReadVersion(path, ver);

        info.sourcePath = path;
        info.fileSize = size;
        info.sha256 = sha;
        info.is64Bit = pe.is64Bit;
        info.fileVersion = ver.fileVersion;
        info.originalFileName = ver.originalFileName;
        info.productName = ver.productName;
        info.companyName = ver.companyName;

        const std::wstring diskName = FileNameOf(path);
        info.kind = KindFromName(ver.originalFileName);
        if (info.kind == ModelKind::Unknown) info.kind = KindFromName(diskName);
        info.displayName = DisplayNameFor(info, diskName);
        info.signature = CheckSignature(path, info.signer);
        return true;
    } catch (const std::bad_alloc&) {
        error = L"Out of memory while inspecting the file.";
    } catch (const std::exception& e) {
        error = L"Unexpected error while inspecting the file: " + FromUtf8(e.what());
    }
    MSR_LOG_ERROR(L"Model inspect failed: {}", error);
    return false;
}

ImportResult ModelStore::Import(const std::wstring& path, ModelInfo& info, std::wstring& error) {
    try {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            error = L"The file was not found.";
            return ImportResult::FileNotFound;
        }
        ModelInfo cand;
        if (!Inspect(path, cand, error)) {
            MSR_LOG_WARN(L"Model import rejected '{}': {}", path, error);
            return ImportResult::NotAModel;
        }
        if (!cand.is64Bit) {
            error = L"This is not a 64-bit library. MoonSR can only use 64-bit models.";
            MSR_LOG_WARN(L"Model import rejected '{}': not 64-bit", path);
            return ImportResult::NotAModel;
        }

        // Already catalogued?
        {
            std::lock_guard lock(g_catalogueMutex);
            for (const Entry& e : LoadCatalogue(directory_)) {
                if (e.info.sha256 == cand.sha256 && FileExists(StoredPath(directory_, e))) {
                    info = e.info;
                    error = L"This model is already installed.";
                    return ImportResult::AlreadyInstalled;
                }
            }
        }

        Entry entry;
        entry.info = cand;
        entry.info.id = KindToken(cand.kind) + L"-" + cand.sha256.substr(0, 16);
        entry.storedName = FileNameOf(path);
        if (!NameIsSafe(entry.storedName)) entry.storedName = L"model.dll";
        entry.info.importedAt = NowIso();

        const std::wstring folder = directory_ + L"\\" + entry.info.id;
        const std::wstring finalPath = folder + L"\\" + entry.storedName;
        const std::wstring tmpPath = finalPath + L".tmp";
        CreateDirectoriesRecursive(folder);
        if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) {
            error = L"Cannot create the model folder: " + Win32Message(GetLastError());
            MSR_LOG_ERROR(L"Model import: {}", error);
            return ImportResult::CopyFailed;
        }
        if (!CopyFileW(path.c_str(), tmpPath.c_str(), FALSE)) {
            error = L"Copying the file failed: " + Win32Message(GetLastError());
            MSR_LOG_ERROR(L"Model import: {}", error);
            return ImportResult::CopyFailed;
        }
        std::wstring copyHash, herr;
        if (!Sha256File(tmpPath, copyHash, nullptr, herr) || copyHash != cand.sha256) {
            error = herr.empty() ? L"The copy does not match the original (the file changed while it was being copied)." : herr;
            MSR_LOG_ERROR(L"Model import: {}", error);
            DeleteFileW(tmpPath.c_str());
            RemoveDirectoryW(folder.c_str());
            return ImportResult::CopyFailed;
        }
        // The stored copy must not stay read-only inherited from the source (Remove has to delete it).
        SetFileAttributesW(tmpPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!MoveFileExW(tmpPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            error = L"Could not finish the copy: " + Win32Message(GetLastError());
            MSR_LOG_ERROR(L"Model import: {}", error);
            DeleteFileW(tmpPath.c_str());
            RemoveDirectoryW(folder.c_str());
            return ImportResult::CopyFailed;
        }
        entry.info.storedPath = finalPath;

        {
            std::lock_guard lock(g_catalogueMutex);
            std::vector<Entry> list = LoadCatalogue(directory_);
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](const Entry& e) { return e.info.id == entry.info.id; }),
                       list.end());
            list.push_back(entry);
            if (!SaveCatalogue(directory_, list, error)) {
                MSR_LOG_ERROR(L"Model import: {}", error);
                DeleteFileW(finalPath.c_str());
                RemoveDirectoryW(folder.c_str());
                return ImportResult::StoreError;
            }
        }
        info = entry.info;
        MSR_LOG_INFO(L"Model imported: {} ({}), signature {}", info.displayName, info.id, static_cast<unsigned>(info.signature));
        return ImportResult::Ok;
    } catch (const std::bad_alloc&) {
        error = L"Out of memory while importing the file.";
    } catch (const std::exception& e) {
        error = L"Unexpected error while importing the file: " + FromUtf8(e.what());
    }
    MSR_LOG_ERROR(L"Model import failed: {}", error);
    return ImportResult::CopyFailed;
}

std::vector<ModelInfo> ModelStore::List() const {
    std::vector<ModelInfo> out;
    try {
        std::lock_guard lock(g_catalogueMutex);
        const std::vector<Entry> list = LoadCatalogue(directory_);
        for (auto it = list.rbegin(); it != list.rend(); ++it) {  // file order is oldest first
            if (FileExists(StoredPath(directory_, *it))) out.push_back(it->info);
        }
    } catch (const std::exception& e) {
        MSR_LOG_ERROR(L"Model list failed: {}", FromUtf8(e.what()));
        out.clear();
    }
    return out;
}

bool ModelStore::Verify(const std::wstring& id, std::wstring& error) const {
    try {
        Entry found;
        bool have = false;
        {
            std::lock_guard lock(g_catalogueMutex);
            for (Entry& e : LoadCatalogue(directory_)) {
                if (e.info.id == id) {
                    found = e;
                    have = true;
                    break;
                }
            }
        }
        if (!have) {
            error = L"This model is not installed.";
            return false;
        }
        const std::wstring path = StoredPath(directory_, found);
        std::wstring hash;
        if (!Sha256File(path, hash, nullptr, error)) return false;
        if (hash != found.info.sha256) {
            error = L"The stored file has changed since it was added (its checksum no longer matches).";
            MSR_LOG_WARN(L"Model verify failed for {}: checksum mismatch", id);
            return false;
        }
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        error = L"Out of memory while verifying the file.";
    } catch (const std::exception& e) {
        error = L"Unexpected error while verifying the file: " + FromUtf8(e.what());
    }
    return false;
}

bool ModelStore::Remove(const std::wstring& id, std::wstring& error) {
    try {
        std::lock_guard lock(g_catalogueMutex);
        std::vector<Entry> list = LoadCatalogue(directory_);
        auto it = std::find_if(list.begin(), list.end(), [&](const Entry& e) { return e.info.id == id; });
        if (it == list.end()) {
            error = L"This model is not installed.";
            return false;
        }
        const std::wstring path = StoredPath(directory_, *it);
        const std::wstring folder = directory_ + L"\\" + it->info.id;
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (FileExists(path) && !DeleteFileW(path.c_str())) {
            error = L"Cannot delete the stored file: " + Win32Message(GetLastError());
            MSR_LOG_ERROR(L"Model remove: {}", error);
            return false;
        }
        DeleteFileW((path + L".tmp").c_str());
        RemoveDirectoryW(folder.c_str());
        list.erase(it);
        if (!SaveCatalogue(directory_, list, error)) {
            MSR_LOG_ERROR(L"Model remove: {}", error);
            return false;
        }
        MSR_LOG_INFO(L"Model removed: {}", id);
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        error = L"Out of memory while removing the model.";
    } catch (const std::exception& e) {
        error = L"Unexpected error while removing the model: " + FromUtf8(e.what());
    }
    return false;
}

}  // namespace moonsr
