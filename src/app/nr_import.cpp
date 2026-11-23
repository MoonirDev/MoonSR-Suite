#include "nr_import.h"

#include "core/log.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <format>
#include <memory>
#include <utility>

namespace moonsr::app::nr {
namespace {

// ------------------------------------------------------------------------------------------------ small helpers

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool SameName(const std::wstring& a, const std::wstring& b) { return Lower(a) == Lower(b); }

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() && SameName(s.substr(s.size() - suffix.size()), suffix);
}

// Archive-relative names use '/'; folder imports are converted to the same form.
std::wstring BaseName(const std::wstring& name) {
    const size_t cut = name.find_last_of(L"/\\");
    return cut == std::wstring::npos ? name : name.substr(cut + 1);
}

std::wstring DirName(const std::wstring& name) {
    const size_t cut = name.find_last_of(L"/\\");
    return cut == std::wstring::npos ? std::wstring() : name.substr(0, cut);
}

bool IsRegularFile(const std::wstring& path, uint64_t* size = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    if (size) *size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return true;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring WinError(DWORD e) {
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, e, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring s = n && text ? std::wstring(text, n) : std::wstring();
    if (text) LocalFree(text);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s.empty() ? std::format(L"error {}", e) : std::format(L"{} (error {})", s, e);
}

std::wstring Bytes(uint64_t b) {
    if (b >= (1ull << 30)) return std::format(L"{:.1f} GB", static_cast<double>(b) / (1024.0 * 1024.0 * 1024.0));
    return std::format(L"{:.1f} MB", static_cast<double>(b) / (1024.0 * 1024.0));
}

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v == INVALID_HANDLE_VALUE ? nullptr : v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h) CloseHandle(h);
    }
    explicit operator bool() const { return h != nullptr; }
    void Reset() {
        if (h) CloseHandle(h);
        h = nullptr;
    }
};

bool ReadAt(HANDLE f, uint64_t offset, void* out, DWORD bytes) {
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(f, out, bytes, &got, nullptr) && got == bytes;
}

uint64_t FileSizeOf(HANDLE f) {
    LARGE_INTEGER li{};
    return GetFileSizeEx(f, &li) ? static_cast<uint64_t>(li.QuadPart) : 0;
}

Handle OpenRead(const std::wstring& path) {
    return Handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
}

// Deletes a folder MoonSR created (staging, extraction, superseded install). Reparse points are removed, never followed.
void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(path.c_str());
                else DeleteFileW(path.c_str());
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTree(path);
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

uint64_t TreeSize(const std::wstring& dir, int depth = 0) {
    if (depth > 32) return 0;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += TreeSize(dir + L"\\" + name, depth + 1);
        else total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return total;
}

std::wstring UniqueTag() { return std::format(L"{}-{}", GetCurrentProcessId(), GetTickCount64()); }

// A fresh, empty folder under %TEMP%, deleted with everything in it when the object goes away.
class TempDir {
public:
    bool Create(std::wstring& error) {
        wchar_t tmp[MAX_PATH + 1] = {};
        const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
        if (n == 0 || n > MAX_PATH) {
            error = L"The temporary folder could not be found.";
            return false;
        }
        std::wstring base(tmp, n);
        if (!base.empty() && base.back() == L'\\') base.pop_back();
        for (int attempt = 0; attempt < 16; ++attempt) {
            const std::wstring p = std::format(L"{}\\MoonSR-import-{}-{}", base, UniqueTag(), attempt);
            if (CreateDirectoryW(p.c_str(), nullptr)) {  // fails if it already exists: always a fresh folder
                path_ = p;
                return true;
            }
        }
        error = L"A temporary folder could not be created: " + WinError(GetLastError());
        return false;
    }
    ~TempDir() {
        if (!path_.empty()) DeleteTree(path_);
    }
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

// ------------------------------------------------------------------------------------------------ PE inspection

struct PeInfo {
    bool x64Dll = false;
    std::vector<std::string> exports;
};

// Reads the headers and the export names of a DLL without loading it. False = not a 64-bit Windows DLL.
bool InspectPe(const std::wstring& path, PeInfo& info, std::wstring& error) {
    info = {};
    Handle f = OpenRead(path);
    if (!f) {
        error = L"could not be opened: " + WinError(GetLastError());
        return false;
    }
    const uint64_t size = FileSizeOf(f.h);
    IMAGE_DOS_HEADER dos{};
    if (size < sizeof(dos) || !ReadAt(f.h, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        static_cast<uint64_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > size) {
        error = L"is not a Windows program library (DLL)";
        return false;
    }
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadAt(f.h, static_cast<uint64_t>(dos.e_lfanew), &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) {
        error = L"is not a Windows program library (DLL)";
        return false;
    }
    if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = L"is not a 64-bit (x64) library";
        return false;
    }
    if (!(nt.FileHeader.Characteristics & IMAGE_FILE_DLL)) {
        error = L"is a program, not a library (DLL)";
        return false;
    }
    info.x64Dll = true;

    // Export names (best effort: a malformed table just gives no names).
    if (nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return true;
    const IMAGE_DATA_DIRECTORY dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return true;
    const WORD sections = nt.FileHeader.NumberOfSections;
    if (sections == 0 || sections > 96) return true;
    const uint64_t secOffset = static_cast<uint64_t>(dos.e_lfanew) + 4 + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    std::vector<IMAGE_SECTION_HEADER> sec(sections);
    if (!ReadAt(f.h, secOffset, sec.data(), static_cast<DWORD>(sizeof(IMAGE_SECTION_HEADER) * sections))) return true;
    const IMAGE_SECTION_HEADER* home = nullptr;
    for (const auto& s : sec) {
        const DWORD span = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
        if (dir.VirtualAddress >= s.VirtualAddress && dir.VirtualAddress < s.VirtualAddress + span) home = &s;
    }
    if (!home || home->SizeOfRawData == 0 || home->SizeOfRawData > (16u << 20) ||
        static_cast<uint64_t>(home->PointerToRawData) + home->SizeOfRawData > size)
        return true;
    std::vector<unsigned char> raw(home->SizeOfRawData);
    if (!ReadAt(f.h, home->PointerToRawData, raw.data(), home->SizeOfRawData)) return true;
    auto at = [&](DWORD rva, size_t bytes) -> const unsigned char* {
        if (rva < home->VirtualAddress) return nullptr;
        const uint64_t off = rva - home->VirtualAddress;
        return off + bytes <= raw.size() ? raw.data() + off : nullptr;
    };
    const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(at(dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)));
    if (!ed || ed->NumberOfNames > 65536) return true;
    const auto* names = reinterpret_cast<const DWORD*>(at(ed->AddressOfNames, sizeof(DWORD) * ed->NumberOfNames));
    if (!names) return true;
    for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
        const unsigned char* p = at(names[i], 1);
        if (!p) continue;
        const size_t maxLen = static_cast<size_t>(raw.data() + raw.size() - p);
        const size_t len = strnlen(reinterpret_cast<const char*>(p), std::min<size_t>(maxLen, 512));
        if (len < maxLen) info.exports.emplace_back(reinterpret_cast<const char*>(p), len);
    }
    return true;
}

bool HasExport(const PeInfo& pe, const char* name) {
    return std::find(pe.exports.begin(), pe.exports.end(), name) != pe.exports.end();
}

// safetensors: 8-byte little-endian header length, then a JSON object.
bool CheckSafetensors(const std::wstring& path, std::wstring& error) {
    Handle f = OpenRead(path);
    if (!f) {
        error = L"could not be opened: " + WinError(GetLastError());
        return false;
    }
    const uint64_t size = FileSizeOf(f.h);
    unsigned char head[9] = {};
    if (size < 1024 * 1024 || !ReadAt(f.h, 0, head, sizeof(head))) {
        error = L"is too small to be the DLSS5_INTEL weights";
        return false;
    }
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | head[i];
    if (n < 2 || n > (100ull << 20) || 8 + n > size || head[8] != '{') {
        error = L"is not a safetensors file";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ entries

// One file offered by an archive or a folder. `name` is relative and '/'-separated.
struct Entry {
    std::wstring name;
    uint64_t size = 0;
    std::wstring source;  // folder import: full path; archive import: filled after extraction
};

struct Listing {
    std::vector<Entry> files;
    uint64_t total = 0;
};

// Rejects names that could escape the extraction folder or that Windows treats specially.
bool SafeName(const std::wstring& name, std::wstring& why) {
    if (name.empty() || name.size() > 512) {
        why = L"an empty or very long file name";
        return false;
    }
    if (name[0] == L'/' || name[0] == L'\\') {
        why = L"an absolute path (" + name + L")";
        return false;
    }
    for (wchar_t c : name) {
        if (c < 0x20 || c == L':' || c == L'"' || c == L'<' || c == L'>' || c == L'|') {
            why = L"a file name with a drive letter or a character Windows does not allow (" + name + L")";
            return false;
        }
    }
    size_t pos = 0;
    while (pos <= name.size()) {
        size_t end = name.find_first_of(L"/\\", pos);
        if (end == std::wstring::npos) end = name.size();
        const std::wstring part = name.substr(pos, end - pos);
        if (part == L"..") {
            why = L"a path that points outside the archive (" + name + L")";
            return false;
        }
        pos = end + 1;
    }
    return true;
}

uint16_t U16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const unsigned char* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t U64(const unsigned char* p) { return static_cast<uint64_t>(U32(p)) | (static_cast<uint64_t>(U32(p + 4)) << 32); }

// Reads a zip archive's central directory: every entry is checked, whatever is selected later.
bool ListZip(const std::wstring& path, Listing& out, std::wstring& error) {
    out = {};
    Handle f = OpenRead(path);
    if (!f) {
        error = L"The file could not be opened: " + WinError(GetLastError());
        return false;
    }
    const uint64_t size = FileSizeOf(f.h);
    if (size < 22) {
        error = L"This file is not a zip archive.";
        return false;
    }
    const DWORD tail = static_cast<DWORD>(std::min<uint64_t>(size, 22 + 65535));
    std::vector<unsigned char> buf(tail);
    if (!ReadAt(f.h, size - tail, buf.data(), tail)) {
        error = L"The archive could not be read.";
        return false;
    }
    int64_t eocd = -1;
    for (int64_t i = static_cast<int64_t>(tail) - 22; i >= 0; --i)
        if (U32(&buf[static_cast<size_t>(i)]) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) {
        error = L"This file is not a zip archive (or it is damaged).";
        return false;
    }
    const unsigned char* e = &buf[static_cast<size_t>(eocd)];
    uint64_t count = U16(e + 10), cdSize = U32(e + 12), cdOffset = U32(e + 16);
    if (U16(e + 4) != 0 || U16(e + 6) != 0) {
        error = L"Split (multi-part) zip archives are not supported.";
        return false;
    }
    if (count == 0xFFFF || cdSize == 0xFFFFFFFF || cdOffset == 0xFFFFFFFF) {  // zip64
        const uint64_t eocdAbs = size - tail + static_cast<uint64_t>(eocd);
        unsigned char loc[20] = {};
        unsigned char z[56] = {};
        if (eocdAbs < 20 || !ReadAt(f.h, eocdAbs - 20, loc, 20) || U32(loc) != 0x07064b50 ||
            !ReadAt(f.h, U64(loc + 8), z, 56) || U32(z) != 0x06064b50) {
            error = L"The zip archive is damaged (zip64 record missing).";
            return false;
        }
        count = U64(z + 32);
        cdSize = U64(z + 40);
        cdOffset = U64(z + 48);
    }
    if (count > kMaxEntries) {
        error = std::format(L"The archive has {} entries; MoonSR accepts at most {}.", count, kMaxEntries);
        return false;
    }
    if (cdSize > (64u << 20) || cdOffset + cdSize > size) {
        error = L"The zip archive is damaged (central directory out of range).";
        return false;
    }
    std::vector<unsigned char> cd(static_cast<size_t>(cdSize));
    if (cdSize && !ReadAt(f.h, cdOffset, cd.data(), static_cast<DWORD>(cdSize))) {
        error = L"The archive could not be read.";
        return false;
    }
    size_t p = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || U32(&cd[p]) != 0x02014b50) {
            error = L"The zip archive is damaged (bad central directory entry).";
            return false;
        }
        const unsigned char* h = &cd[p];
        const uint16_t madeBy = U16(h + 4), flags = U16(h + 8), nameLen = U16(h + 28), extraLen = U16(h + 30),
                       commentLen = U16(h + 32);
        uint64_t usize = U32(h + 24);
        const uint32_t extAttr = U32(h + 38);
        if (p + 46 + nameLen + extraLen + commentLen > cd.size()) {
            error = L"The zip archive is damaged (entry out of range).";
            return false;
        }
        const char* rawName = reinterpret_cast<const char*>(h + 46);
        const UINT cp = (flags & 0x0800) ? CP_UTF8 : 437;
        const int wn = MultiByteToWideChar(cp, 0, rawName, nameLen, nullptr, 0);
        std::wstring name(static_cast<size_t>(std::max(wn, 0)), L'\0');
        if (wn > 0) MultiByteToWideChar(cp, 0, rawName, nameLen, name.data(), wn);
        // zip64 extra field: the 64-bit sizes replace the 0xFFFFFFFF placeholders, in order.
        const unsigned char* x = h + 46 + nameLen;
        for (size_t xo = 0; xo + 4 <= extraLen;) {
            const uint16_t id = U16(x + xo), len = U16(x + xo + 2);
            if (xo + 4 + len > extraLen) break;
            if (id == 0x0001 && usize == 0xFFFFFFFF && len >= 8) usize = U64(x + xo + 4);
            xo += 4 + len;
        }
        std::wstring why;
        if (!SafeName(name, why)) {
            error = L"This archive was rejected because it contains " + why + L". Nothing was installed.";
            return false;
        }
        const bool unixSymlink = (madeBy >> 8) == 3 && ((extAttr >> 16) & 0170000) == 0120000;
        if (unixSymlink) {
            error = L"This archive was rejected because it contains a symbolic link (" + name + L"). Nothing was installed.";
            return false;
        }
        const bool isDir = name.back() == L'/' || name.back() == L'\\';
        if (!isDir) {
            if (flags & 0x0001) {
                error = L"This archive is password-protected (" + name + L"). Extract it yourself and use Choose files / Choose folder.";
                return false;
            }
            out.total += usize;
            if (out.total > kMaxArchiveBytes) {
                error = L"The archive would expand to more than " + Bytes(kMaxArchiveBytes) + L". Nothing was installed.";
                return false;
            }
            out.files.push_back({name, usize, {}});
        }
        p += 46u + nameLen + extraLen + commentLen;
    }
    return true;
}

// Lists the regular files of a folder (up to 4 levels deep), skipping links.
bool ListFolder(const std::wstring& root, Listing& out, std::wstring& error) {
    out = {};
    std::vector<std::pair<std::wstring, int>> todo{{L"", 0}};
    while (!todo.empty()) {
        auto [rel, depth] = todo.back();
        todo.pop_back();
        const std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            const std::wstring r = rel.empty() ? name : rel + L"/" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (depth < 4) todo.push_back({r, depth + 1});
                continue;
            }
            const uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            out.files.push_back({r, sz, dir + L"\\" + name});
            if (out.files.size() > kMaxEntries) {
                FindClose(find);
                error = std::format(L"The folder has more than {} files. Choose the folder that holds d5c_chain.dll.", kMaxEntries);
                return false;
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ extraction (tar.exe)

// Extracts only `wanted` from the zip into `into` with Windows' own tar.exe (bsdtar, Windows 10 1803 and later). bsdtar
// refuses absolute and ".." paths by default; the listing has already rejected them. The extracted size is watched and
// the process is killed if it grows past what the central directory declared.
bool ExtractWithTar(const std::wstring& zip, const std::vector<Entry*>& wanted, const std::wstring& into,
                    const std::wstring& errFile, std::wstring& error) {
    wchar_t sys[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        error = L"The Windows system folder could not be found.";
        return false;
    }
    const std::wstring tar = std::wstring(sys, n) + L"\\tar.exe";
    if (!IsRegularFile(tar)) {
        error = L"Windows' built-in tar.exe is missing (it ships with Windows 10 1803 and later). Extract the zip yourself "
                L"and use Choose files / Choose folder instead.";
        return false;
    }
    std::wstring cmd = L"\"" + tar + L"\" -x -f \"" + zip + L"\" -C \"" + into + L"\" --";
    uint64_t declared = 0;
    for (const Entry* e : wanted) {
        if (e->name.find_first_of(L"*?[") != std::wstring::npos) {
            error = L"The archive has a file name tar cannot extract safely (" + e->name + L"). Extract it yourself and "
                    L"use Choose files / Choose folder.";
            return false;
        }
        std::wstring member = e->name;  // bsdtar reports and matches zip names with '/' even when stored with '\\'
        std::replace(member.begin(), member.end(), L'\\', L'/');
        cmd += L" \"" + member + L"\"";
        declared += e->size;
    }
    if (cmd.size() > 30000) {
        error = L"Too many files to extract in one step. Extract the archive yourself and use Choose folder.";
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle err(CreateFileW(errFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle nul(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!err || !nul || !job) {
        error = L"Extraction could not be prepared: " + WinError(GetLastError());
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &lim, sizeof(lim));

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul.h;
    si.hStdOutput = err.h;
    si.hStdError = err.h;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    if (!CreateProcessW(tar.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        into.c_str(), &si, &pi)) {
        error = L"tar.exe could not be started: " + WinError(GetLastError());
        return false;
    }
    Handle proc(pi.hProcess), thread(pi.hThread);
    AssignProcessToJobObject(job.h, proc.h);
    ResumeThread(thread.h);

    const ULONGLONG deadline = GetTickCount64() + 10 * 60 * 1000;
    const uint64_t allowed = declared + (1u << 20);
    for (;;) {
        const DWORD w = WaitForSingleObject(proc.h, 200);
        if (w == WAIT_OBJECT_0) break;
        if (TreeSize(into) > allowed) {
            TerminateJobObject(job.h, 1);
            WaitForSingleObject(proc.h, 5000);
            error = L"The archive expands to more data than it declares, so it was rejected. Nothing was installed.";
            return false;
        }
        if (GetTickCount64() > deadline) {
            TerminateJobObject(job.h, 1);
            WaitForSingleObject(proc.h, 5000);
            error = L"Extracting the archive took longer than 10 minutes and was stopped. Nothing was installed.";
            return false;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(proc.h, &code);
    if (code != 0) {
        std::wstring detail;
        err.Reset();  // close our write handle so the log can be read
        if (Handle r{CreateFileW(errFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)}) {
            char text[512] = {};
            DWORD got = 0;
            if (ReadFile(r.h, text, sizeof(text) - 1, &got, nullptr) && got > 0) {
                std::string s(text, got);
                const size_t nl = s.find_first_of("\r\n");
                if (nl != std::string::npos) s.resize(nl);
                detail = L": " + std::wstring(s.begin(), s.end());
            }
        }
        error = std::format(L"Windows could not extract the archive (tar exit code {}{}). Nothing was installed.", code, detail);
        return false;
    }
    // Every wanted file must now exist as a plain file of the declared size, with no link anywhere on its path.
    for (Entry* e : wanted) {
        std::wstring rel = e->name;
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        std::wstring check = into;
        size_t pos = 0;
        while (pos < rel.size()) {
            size_t end = rel.find(L'\\', pos);
            if (end == std::wstring::npos) end = rel.size();
            check += L"\\" + rel.substr(pos, end - pos);
            const DWORD a = GetFileAttributesW(check.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
                error = L"The archive did not extract as expected (" + e->name + L"). Nothing was installed.";
                return false;
            }
            pos = end + 1;
        }
        uint64_t sz = 0;
        if (!IsRegularFile(check, &sz) || sz != e->size) {
            error = L"The archive did not extract as expected (" + e->name + L"). Nothing was installed.";
            return false;
        }
        e->source = check;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ install

struct Copy {
    std::wstring source;
    std::wstring name;  // file name in the destination folder
};

// Recreates `from` inside `to` with hard links (copies when a link is not possible), skipping `replaced` names at the top.
bool MirrorExisting(const std::wstring& from, const std::wstring& to, const std::vector<std::wstring>& replaced, int depth,
                    std::wstring& error) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (depth == 0 && std::any_of(replaced.begin(), replaced.end(), [&](const std::wstring& r) { return SameName(r, name); }))
            continue;
        const std::wstring src = from + L"\\" + name, dst = to + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth >= 8 || !CreateDirectoryW(dst.c_str(), nullptr) || !MirrorExisting(src, dst, replaced, depth + 1, error)) {
                if (error.empty()) error = L"The current install could not be prepared for the update: " + WinError(GetLastError());
                ok = false;
            }
        } else if (!CreateHardLinkW(dst.c_str(), src.c_str(), nullptr) && !CopyFileW(src.c_str(), dst.c_str(), TRUE)) {
            error = L"The current install could not be prepared for the update (" + name + L"): " + WinError(GetLastError());
            ok = false;
        }
    } while (ok && FindNextFileW(find, &fd));
    FindClose(find);
    return ok;
}

void RemoveStale(const std::wstring& parent, const std::wstring& base) {
    for (const wchar_t* kind : {L".import-", L".old-"}) {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((parent + L"\\" + base + kind + L"*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                DeleteTree(parent + L"\\" + fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
}

// Builds the new install in a staging folder beside `destination` (existing files kept, the imported ones replaced) and
// swaps it in with two renames. Any failure before the swap leaves `destination` untouched.
bool Install(const std::wstring& destination, const std::vector<Copy>& files,
             const std::function<void(const std::wstring&)>& progress, std::wstring& error) {
    const std::wstring parent = DirName(destination), base = BaseName(destination);
    if (parent.empty() || base.empty() || !IsDirectory(parent)) {
        error = L"The MoonSR folder could not be found.";
        return false;
    }
    RemoveStale(parent, base);
    uint64_t need = 0;
    for (const Copy& c : files) {
        uint64_t sz = 0;
        IsRegularFile(c.source, &sz);
        need += sz;
    }
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(parent.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < need + (64ull << 20)) {
        error = L"There is not enough free disk space: " + Bytes(need) + L" is needed in " + parent + L".";
        return false;
    }
    const std::wstring tag = UniqueTag();
    const std::wstring staging = parent + L"\\" + base + L".import-" + tag;
    const std::wstring old = parent + L"\\" + base + L".old-" + tag;
    if (!CreateDirectoryW(staging.c_str(), nullptr)) {
        const DWORD e = GetLastError();
        error = e == ERROR_ACCESS_DENIED
                    ? L"MoonSR cannot write to its folder (" + parent + L"). Run MoonSR from a folder you can write to, or "
                      L"copy the files into " + destination + L" yourself."
                    : L"A staging folder could not be created: " + WinError(e);
        return false;
    }
    struct StagingGuard {
        std::wstring path;
        bool keep = false;
        ~StagingGuard() {
            if (!keep) DeleteTree(path);
        }
    } guard{staging};

    const bool existed = IsDirectory(destination);
    std::vector<std::wstring> replaced;
    for (const Copy& c : files) replaced.push_back(c.name);
    if (existed && !MirrorExisting(destination, staging, replaced, 0, error)) return false;
    for (const Copy& c : files) {
        if (progress) progress(L"Copying " + c.name);
        const std::wstring dst = staging + L"\\" + c.name;
        if (!CopyFileW(c.source.c_str(), dst.c_str(), TRUE)) {
            error = L"Copying " + c.name + L" failed: " + WinError(GetLastError());
            return false;
        }
        SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
        uint64_t a = 0, b = 0;
        if (!IsRegularFile(c.source, &a) || !IsRegularFile(dst, &b) || a != b) {
            error = L"Copying " + c.name + L" did not complete.";
            return false;
        }
    }
    if (progress) progress(L"Finishing");
    if (existed && !MoveFileExW(destination.c_str(), old.c_str(), 0)) {
        const DWORD e = GetLastError();
        error = (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION)
                    ? L"The files in " + destination + L" are in use. Stop scaling (and close other programs using them), then try again."
                    : L"The current install could not be replaced: " + WinError(e);
        return false;
    }
    if (!MoveFileExW(staging.c_str(), destination.c_str(), 0)) {
        const DWORD e = GetLastError();
        if (existed) MoveFileExW(old.c_str(), destination.c_str(), 0);  // put the previous install back
        error = L"The new files could not be put in place: " + WinError(e);
        return false;
    }
    guard.keep = true;
    if (existed) DeleteTree(old);
    return true;
}

// ------------------------------------------------------------------------------------------------ selection

const wchar_t kAmdDll[] = L"LmxxfNrRuntime.dll";
const wchar_t kAmdPak[] = L"LmxxfNrRuntime.pak";
const wchar_t kIntelDll[] = L"d5c_chain.dll";
const wchar_t kIntelWeights[] = L"dlssnr-logical.safetensors";
const char* const kIntelExports[] = {"dlss5_create", "dlss5_destroy", "dlss5_process_frame", "dlss5_set_scale",
                                     "dlss5_set_phase30", "dlss5_reset_history"};

std::vector<Entry*> Named(Listing& l, const std::wstring& base) {
    std::vector<Entry*> out;
    for (Entry& e : l.files)
        if (SameName(BaseName(e.name), base)) out.push_back(&e);
    return out;
}

std::wstring JoinNames(const std::vector<Entry*>& v) {
    std::wstring s;
    for (const Entry* e : v) s += (s.empty() ? L"" : L", ") + e->name;
    return s;
}

bool SelectAmd(Listing& l, const std::wstring& what, std::vector<Entry*>& pick, std::wstring& error) {
    const auto dlls = Named(l, kAmdDll);
    if (dlls.empty()) {
        error = L"This " + what + L" doesn't contain LmxxfNrRuntime.dll. Choose the AMDNR release zip (AMDNR-v0.3.5.6.zip).";
        return false;
    }
    if (dlls.size() > 1) {
        error = L"This " + what + L" contains more than one LmxxfNrRuntime.dll (" + JoinNames(dlls) +
                L"). Extract the one you want and use Choose files.";
        return false;
    }
    const std::wstring dir = Lower(DirName(dlls[0]->name));
    Entry* pak = nullptr;
    for (Entry* e : Named(l, kAmdPak))
        if (Lower(DirName(e->name)) == dir) pak = e;
    if (!pak) {
        error = L"This " + what + L" has LmxxfNrRuntime.dll but not LmxxfNrRuntime.pak beside it. Both come from the same "
                L"AMDNR release (AMDNR-v0.3.5.6.zip).";
        return false;
    }
    pick = {dlls[0], pak};
    for (Entry& e : l.files)  // runtime DLL dependencies shipped in the same folder of the release
        if (&e != dlls[0] && Lower(DirName(e.name)) == dir && EndsWithNoCase(e.name, L".dll")) pick.push_back(&e);
    return true;
}

bool SelectIntel(Listing& l, const std::wstring& what, std::vector<Entry*>& pick, std::wstring& error) {
    const auto chains = Named(l, kIntelDll);
    if (chains.empty()) {
        error = L"This " + what + L" doesn't contain d5c_chain.dll. Choose the DLSS5_INTEL bundle you built or downloaded "
                L"from that project (d5c_chain.dll with its .spv shaders).";
        return false;
    }
    if (chains.size() > 1) {
        error = L"This " + what + L" contains more than one d5c_chain.dll (" + JoinNames(chains) +
                L"). Choose the folder that holds the one you want.";
        return false;
    }
    const std::wstring dir = Lower(DirName(chains[0]->name));
    std::vector<Entry*> spv;
    for (Entry& e : l.files)
        if (EndsWithNoCase(e.name, L".spv") && Lower(DirName(e.name)) == dir) spv.push_back(&e);
    if (spv.empty()) {  // shaders in a subfolder of the runtime's folder
        const std::wstring prefix = dir.empty() ? std::wstring() : dir + L"/";
        for (Entry& e : l.files)
            if (EndsWithNoCase(e.name, L".spv") && Lower(e.name).rfind(prefix, 0) == 0) spv.push_back(&e);
    }
    if (spv.empty()) {
        error = L"This " + what + L" has d5c_chain.dll but no .spv compute shaders. The shaders (gemm.spv and the others) "
                L"come with the runtime; take them from the same build.";
        return false;
    }
    for (size_t i = 0; i < spv.size(); ++i)
        for (size_t j = i + 1; j < spv.size(); ++j)
            if (SameName(BaseName(spv[i]->name), BaseName(spv[j]->name))) {
                error = L"This " + what + L" has two shaders with the same name (" + spv[i]->name + L", " + spv[j]->name +
                        L"). Choose the folder of one build.";
                return false;
            }
    if (std::none_of(spv.begin(), spv.end(), [](const Entry* e) { return SameName(BaseName(e->name), L"gemm.spv"); })) {
        error = L"This " + what + L" has .spv shaders but not gemm.spv, which the runtime needs.";
        return false;
    }
    pick = {chains[0]};
    pick.insert(pick.end(), spv.begin(), spv.end());
    const auto weights = Named(l, kIntelWeights);
    if (weights.size() > 1) {
        error = L"This " + what + L" contains more than one dlssnr-logical.safetensors (" + JoinNames(weights) + L").";
        return false;
    }
    if (weights.size() == 1) pick.push_back(weights[0]);
    return true;
}

std::wstring JoinFiles(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (const auto& n : v) s += (s.empty() ? L"" : L", ") + n;
    return s;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ GPU support

// AMD architecture by PCI device ID (vendor 0x1002). Source: the PCI ID repository, pci.ids version 2026.11.22
// (https://raw.githubusercontent.com/pciutils/pciids/master/pci.ids, AMD section, read 2026-11-22). Ranges are the
// blocks AMD uses per die; the IDs listed in pci.ids at that date are:
//   Navi 31  7446 7448 7449 744a 744b 744c 745e          (RX 7900 series, PRO W7900/W7800)       RDNA 3
//   Navi 32  7460 7461 7470 747e                          (RX 7700 XT / 7800 XT, PRO W7700)       RDNA 3
//   Navi 33  73f0 7480 7481 7483 7487 7489 748b 7499 749f (RX 7600 / 7500 / 7400 / 7300, W7600)   RDNA 3
//   Navi 44  7590                                         (RX 9050 / 9060 XT)                     RDNA 4
//   Navi 48  7550 7551                                    (RX 9070 series, AI PRO R9700)          RDNA 4
//   Phoenix 15bf 15c8 164f, HawkPoint 1900 1901, Strix 150e, Strix Halo 1586, Krackan 1114 1902   RDNA 3 / 3.5 APUs
//   Navi 21 73a1-73bf, Navi 22 73c3-73df, Navi 23 73e0-73ff (except 73f0), Navi 24 7421-743f      RDNA 2
//   Raphael 164e, Granite Ridge 13c0, Mendocino 1506, Rembrandt 164d 1681, VanGogh 163f           RDNA 2 APUs
//   Navi 10 / 12 / 14  7310-7362                                                                  RDNA 1
// AMDNR (NR_BRIDGE.md) runs on RDNA 3, RDNA 4 and the RDNA 3-class APUs. To enable RX 6000 once a suitable AMDNR runtime
// exists, set `supported` for the RDNA 2 rows below. Anything not in the table is reported as Unknown, never as unsupported.
GpuSupport ClassifyAmd(uint32_t id) {
    struct Range {
        uint32_t first, last;
        const wchar_t* family;
        bool supported;
    };
    static constexpr Range kTable[] = {
        {0x73f0, 0x73f0, L"RDNA 3 (Navi 33)", true},  // listed under Navi 33 although it sits in the Navi 23 block
        {0x7440, 0x745f, L"RDNA 3 (Navi 31)", true},
        {0x7460, 0x747f, L"RDNA 3 (Navi 32)", true},
        {0x7480, 0x749f, L"RDNA 3 (Navi 33)", true},
        {0x7550, 0x755f, L"RDNA 4 (Navi 48)", true},
        {0x7590, 0x759f, L"RDNA 4 (Navi 44)", true},
        {0x15bf, 0x15bf, L"RDNA 3 integrated (Phoenix)", true},
        {0x15c8, 0x15c8, L"RDNA 3 integrated (Phoenix)", true},
        {0x164f, 0x164f, L"RDNA 3 integrated (Phoenix)", true},
        {0x1900, 0x1901, L"RDNA 3 integrated (Hawk Point)", true},
        {0x150e, 0x150e, L"RDNA 3.5 integrated (Strix Point)", true},
        {0x1586, 0x1586, L"RDNA 3.5 integrated (Strix Halo)", true},
        {0x1114, 0x1114, L"RDNA 3.5 integrated (Krackan Point)", true},
        {0x1902, 0x1902, L"RDNA 3.5 integrated (Krackan Point)", true},
        {0x73a0, 0x73bf, L"RDNA 2 (Navi 21)", false},
        {0x73c0, 0x73df, L"RDNA 2 (Navi 22)", false},
        {0x73e0, 0x73ff, L"RDNA 2 (Navi 23)", false},
        {0x7420, 0x743f, L"RDNA 2 (Navi 24)", false},
        {0x164e, 0x164e, L"RDNA 2 integrated (Raphael)", false},
        {0x13c0, 0x13c0, L"RDNA 2 integrated (Granite Ridge)", false},
        {0x1506, 0x1506, L"RDNA 2 integrated (Mendocino)", false},
        {0x164d, 0x164d, L"RDNA 2 integrated (Rembrandt)", false},
        {0x1681, 0x1681, L"RDNA 2 integrated (Rembrandt)", false},
        {0x163f, 0x163f, L"RDNA 2 integrated (Van Gogh)", false},
        {0x7310, 0x736f, L"RDNA 1 (Navi 1x)", false},
    };
    for (const Range& r : kTable) {  // first match wins (0x73f0 before the Navi 23 block)
        if (id < r.first || id > r.last) continue;
        GpuSupport s;
        s.family = r.family;
        s.support = r.supported ? Support::Supported : Support::NotSupported;
        if (!r.supported) s.reason = L"AMDNR supports Radeon RX 7000 and RX 9000 series and newer; this GPU is " + s.family + L".";
        return s;
    }
    GpuSupport s;
    s.support = Support::Unknown;
    s.family = std::format(L"Radeon (device {:04X})", id);
    s.reason = L"Unknown Radeon: AMDNR may not support it. It runs on Radeon RX 7000 and RX 9000 series.";
    return s;
}

// NVIDIA architecture by PCI device ID (vendor 0x10DE). Source: the PCI ID repository, pci.ids (read 2026-11-22 from
// https://raw.githubusercontent.com/pciutils/pciids/master/pci.ids, vendor 10de). Block per chip family:
//   0x1E00-0x1FFF  TU102/TU104/TU106/TU117 (RTX 20, Quadro RTX, GTX 16)    Turing    e.g. 1e87 RTX 2080, 1e04 RTX 2080 Ti
//   0x2180-0x21FF  TU116 (GTX 1650/1660)                                   Turing
//   0x2200-0x22FF  GA102 (RTX 3080/3090, RTX A6000/A5000)                  Ampere    e.g. 2206 RTX 3080, 2230 RTX A6000
//   0x2400-0x25FF  GA103/GA104/GA106/GA107 (RTX 3050-3070 Ti, A400-A4000)  Ampere    e.g. 25b2 RTX A400, 2571 RTX A2000
//   0x2600-0x28FF  AD102-AD107 (RTX 4060-4090, RTX 2000-6000 Ada)          Ada       e.g. 2684 RTX 4090, 26b1 RTX 6000 Ada
//   0x2B00-0x2FFF  GB202/GB203/GB205/GB206/GB207 (RTX 50, RTX PRO)         Blackwell e.g. 2b85 RTX 5090, 2d05 RTX 5060
// Deliberately Unknown: 0x2080-0x20FF GA100 (A100), 0x2300-0x23FF GH100 (Hopper H100/H200: the brief's Ampere range
// 0x2200-0x25FF would include them, so it is split), 0x2900-0x2AFF GB100/GB102 (data centre), 0x3000 and up GR10x.
// The 22xx block also holds HD Audio functions (22ba-22ec); those are never display adapters. GTX parts inside these
// blocks (TU117, TU116) are rejected earlier because ClassifyGpu requires "RTX" in the name.
NvidiaArch ClassifyNvidia(uint32_t id, const std::wstring& name) {
    struct Range {
        uint32_t first, last;
        NvidiaArch arch;
    };
    static constexpr Range kTable[] = {
        {0x1e00, 0x1fff, NvidiaArch::Turing},    {0x2180, 0x21ff, NvidiaArch::Turing},
        {0x2200, 0x22ff, NvidiaArch::Ampere},    {0x2400, 0x25ff, NvidiaArch::Ampere},
        {0x2600, 0x28ff, NvidiaArch::Ada},       {0x2b00, 0x2fff, NvidiaArch::Blackwell},
    };
    for (const Range& r : kTable)
        if (id >= r.first && id <= r.last) return r.arch;
    if (id != 0 && ((id >= 0x2080 && id <= 0x20ff) || (id >= 0x2300 && id <= 0x23ff) || (id >= 0x2900 && id <= 0x2aff) || id >= 0x3000))
        return NvidiaArch::Unknown;  // known data-centre blocks and newer than this table: do not guess from the name
    // The ID is not in the table (older or missing): use the marketing name.
    auto has = [&](const wchar_t* t) { return name.find(t) != std::wstring::npos; };
    if (has(L" Ada") || has(L"Ada Generation")) return NvidiaArch::Ada;
    if (has(L"Blackwell") || has(L"RTX PRO")) return NvidiaArch::Blackwell;
    if (has(L"Quadro RTX") || has(L"TITAN RTX")) return NvidiaArch::Turing;
    if (has(L"RTX A")) return NvidiaArch::Ampere;
    const size_t p = name.find(L"RTX ");
    if (p != std::wstring::npos && p + 4 < name.size() && std::iswdigit(name[p + 4])) {
        switch (name[p + 4]) {
        case L'2': return NvidiaArch::Turing;
        case L'3': return NvidiaArch::Ampere;
        case L'4': return NvidiaArch::Ada;
        case L'5': return NvidiaArch::Blackwell;
        default: break;
        }
    }
    return NvidiaArch::Unknown;
}

const wchar_t* NvidiaArchName(NvidiaArch a) {
    switch (a) {
    case NvidiaArch::Turing: return L"Turing (RTX 20 series)";
    case NvidiaArch::Ampere: return L"Ampere (RTX 30 series)";
    case NvidiaArch::Ada: return L"Ada Lovelace (RTX 40 series)";
    case NvidiaArch::Blackwell: return L"Blackwell (RTX 50 series)";
    default: return L"";
    }
}

namespace {
// Known FP16-patched nvngx_dlssnr.dll builds (SHA-256, lowercase). Add further builds here.
constexpr const wchar_t* kFp16RuntimeHashes[] = {
    L"cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed",
};
std::vector<std::wstring>& TestFp16Hashes() {
    static std::vector<std::wstring> v;
    return v;
}
}  // namespace

bool IsFp16RuntimeHash(const std::wstring& sha256) {
    for (const wchar_t* h : kFp16RuntimeHashes)
        if (_wcsicmp(sha256.c_str(), h) == 0) return true;
    for (const std::wstring& h : TestFp16Hashes())
        if (_wcsicmp(sha256.c_str(), h.c_str()) == 0) return true;
    return false;
}

void AddTestFp16Hash(const std::wstring& sha256) { TestFp16Hashes().push_back(sha256); }

GpuSupport ClassifyGpu(const AdapterInfo& a) {
    GpuSupport s;
    const std::wstring name = a.name;
    if (a.vendor == Vendor::Nvidia) {
        s.family = L"RTX";
        if (name.find(L"RTX") != std::wstring::npos) {
            s.support = Support::Supported;
            s.nvidia = ClassifyNvidia(a.deviceId, name);
            if (s.nvidia != NvidiaArch::Unknown) s.family = NvidiaArchName(s.nvidia);
        } else {
            s.support = Support::NotSupported;
            s.reason = L"Neural rendering needs an NVIDIA GeForce RTX card.";
            s.family = L"NVIDIA";
        }
        return s;
    }
    if (a.vendor == Vendor::Amd) return ClassifyAmd(a.deviceId);
    if (a.vendor == Vendor::Intel) {
        // "Intel(R) Arc(TM) A770 Graphics", "Intel(R) Arc(TM) B580 Graphics", "Intel(R) Arc(TM) Pro B50 Graphics".
        const size_t arc = name.find(L"Arc");
        if (arc == std::wstring::npos) {
            s.support = Support::NotSupported;
            s.family = L"Intel";
            s.reason = L"Neural rendering needs an Intel Arc A- or B-series GPU.";
            return s;
        }
        size_t i = arc + 3;
        if (name.compare(i, 4, L"(TM)") == 0) i += 4;
        while (i < name.size() && name[i] == L' ') ++i;
        if (name.compare(i, 4, L"Pro ") == 0) i += 4;
        const bool series = i + 1 < name.size() && (name[i] == L'A' || name[i] == L'B') && std::iswdigit(name[i + 1]);
        s.family = L"Arc A/B-series";
        if (series) {
            s.support = Support::Supported;
        } else {
            s.support = Support::Unknown;
            s.family = L"Intel Arc";
            s.reason = L"This Arc GPU may not support it: neural rendering needs an Intel Arc A- or B-series GPU.";
        }
        return s;
    }
    s.support = Support::NotSupported;
    s.family = L"Other";
    s.reason = L"Neural rendering needs an NVIDIA GeForce RTX card, an AMD Radeon RX 7000 or RX 9000 series GPU, or an "
               L"Intel Arc A- or B-series GPU.";
    return s;
}

// ------------------------------------------------------------------------------------------------ status

IntelFiles IntelStatus(const std::wstring& folder) {
    IntelFiles f;
    if (folder.empty()) return f;
    f.chain = IsRegularFile(folder + L"\\" + kIntelDll);
    f.shaders = IsRegularFile(folder + L"\\gemm.spv");
    f.weights = IsRegularFile(folder + L"\\" + kIntelWeights);
    return f;
}

bool AmdFilesPresent(const std::wstring& folder) {
    return !folder.empty() && IsRegularFile(folder + L"\\" + kAmdDll) && IsRegularFile(folder + L"\\" + kAmdPak);
}

// ------------------------------------------------------------------------------------------------ probe

int RunProbeChild(const std::wstring& dll, const std::wstring& exportName) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
    const HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) {
        const DWORD e = GetLastError();
        return e ? static_cast<int>(e) : 1;
    }
    std::string name;  // export names are ASCII
    for (wchar_t c : exportName) name.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return GetProcAddress(m, name.c_str()) ? 0 : kProbeNoExport;  // the process exits without calling into the DLL
}

bool ProbeLoad(const std::wstring& dll, const std::string& exportName, std::wstring& error) {
    const std::wstring file = BaseName(dll);
    wchar_t exe[32768] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n == 0 || n >= std::size(exe)) {
        error = L"MoonSR could not start its load check.";
        return false;
    }
    std::wstring cmd = L"\"" + std::wstring(exe, n) + L"\" --nr-probe-load \"" + dll + L"\" " +
                       std::wstring(exportName.begin(), exportName.end());
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        error = L"MoonSR could not start its load check: " + WinError(GetLastError());
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        error = L"MoonSR could not start its load check: " + WinError(GetLastError());
        return false;
    }
    Handle proc(pi.hProcess), thread(pi.hThread);
    AssignProcessToJobObject(job.h, proc.h);
    ResumeThread(thread.h);
    if (WaitForSingleObject(proc.h, 30000) != WAIT_OBJECT_0) {
        TerminateJobObject(job.h, 1);
        error = file + L" did not finish loading within 30 seconds.";
        return false;
    }
    DWORD code = 1;
    GetExitCodeProcess(proc.h, &code);
    MSR_LOG_INFO(L"Neural runtime load check: {} -> {:#x}", dll, code);
    switch (code) {
    case 0: return true;
    case static_cast<DWORD>(kProbeNoExport):
        error = std::format(L"{} loads, but it does not export {}, so it is not the runtime MoonSR expects.", file,
                            std::wstring(exportName.begin(), exportName.end()));
        return false;
    case ERROR_MOD_NOT_FOUND:
        error = file + L" needs a DLL that is not installed (error 126). Install the AMD Software (Adrenalin) driver and "
                       L"the HIP runtime the AMDNR release asks for, and keep any DLLs from the release beside it.";
        return false;
    case ERROR_PROC_NOT_FOUND:
        error = file + L" needs a newer version of a DLL it uses (error 127). Update the AMD driver.";
        return false;
    case ERROR_BAD_EXE_FORMAT:
        error = file + L" is not a valid 64-bit Windows library (error 193).";
        return false;
    case ERROR_DLL_INIT_FAILED:
        error = file + L" refused to start on this PC (error 1114). The GPU or driver may not be supported.";
        return false;
    default:
        if (code >= 0xC0000000u)
            error = std::format(L"{} crashed while loading ({:#010x}). The GPU or driver may not be supported.", file, code);
        else
            error = file + L" could not be loaded: " + WinError(code);
        return false;
    }
}

// ------------------------------------------------------------------------------------------------ import

ImportResult RunImport(ImportKind kind, const std::vector<std::wstring>& paths, const std::wstring& destination,
                       const std::function<void(const std::wstring&)>& progress) {
    ImportResult r;
    if (paths.empty() || destination.empty()) {
        r.message = L"Nothing was chosen.";
        return r;
    }
    const bool amd = kind == ImportKind::AmdArchive || kind == ImportKind::AmdFiles;
    std::vector<Copy> copies;
    TempDir temp;  // extraction folder for archives; removed on every path out of this function
    std::wstring error;

    if (kind == ImportKind::IntelWeights) {
        if (!IsRegularFile(paths[0])) {
            r.message = L"The weights file could not be found.";
            return r;
        }
        if (!CheckSafetensors(paths[0], error)) {
            r.message = BaseName(paths[0]) + L" " + error + L". Choose dlssnr-logical.safetensors, converted by the "
                        L"DLSS5_INTEL tool from your own nvngx_dlssnr.dll. Nothing was installed.";
            return r;
        }
        copies.push_back({paths[0], kIntelWeights});
    } else if (kind == ImportKind::AmdFiles) {
        Listing l;
        for (const std::wstring& p : paths) {
            uint64_t sz = 0;
            if (!IsRegularFile(p, &sz)) {
                r.message = L"This file could not be found: " + p;
                return r;
            }
            const std::wstring b = BaseName(p);
            if (!SameName(b, kAmdPak) && !EndsWithNoCase(b, L".dll")) {
                r.message = L"Only LmxxfNrRuntime.dll, LmxxfNrRuntime.pak and DLLs from the AMDNR release can be added (" + b + L").";
                return r;
            }
            l.files.push_back({b, sz, p});
            l.total += sz;
        }
        // The .pak beside the chosen DLL counts as chosen.
        if (Named(l, kAmdPak).empty() && !Named(l, kAmdDll).empty()) {
            const std::wstring pak = DirName(Named(l, kAmdDll)[0]->source) + L"\\" + kAmdPak;
            uint64_t sz = 0;
            if (IsRegularFile(pak, &sz)) l.files.push_back({kAmdPak, sz, pak});
        }
        std::vector<Entry*> pick;
        if (!SelectAmd(l, L"selection", pick, error)) {
            r.message = Named(l, kAmdDll).empty() ? L"Choose LmxxfNrRuntime.dll and LmxxfNrRuntime.pak from the AMDNR release (AMDNR-v0.3.5.6.zip)."
                                                  : L"LmxxfNrRuntime.pak was not chosen and is not beside LmxxfNrRuntime.dll. Choose both files from the AMDNR release.";
            return r;
        }
        for (const Entry* e : pick) copies.push_back({e->source, BaseName(e->name)});
    } else {
        const bool archive = kind == ImportKind::AmdArchive || kind == ImportKind::IntelArchive;
        const std::wstring what = archive ? L"zip" : L"folder";
        Listing l;
        if (progress) progress(archive ? L"Checking the archive" : L"Reading the folder");
        if (archive ? !ListZip(paths[0], l, error) : !ListFolder(paths[0], l, error)) {
            r.message = error;
            return r;
        }
        std::vector<Entry*> pick;
        if (amd ? !SelectAmd(l, what, pick, error) : !SelectIntel(l, what, pick, error)) {
            r.message = error;
            return r;
        }
        uint64_t total = 0;
        for (const Entry* e : pick) total += e->size;
        if (total > kMaxArchiveBytes) {
            r.message = L"The selected files are larger than " + Bytes(kMaxArchiveBytes) + L". Nothing was installed.";
            return r;
        }
        if (archive) {
            if (!temp.Create(error)) {
                r.message = error;
                return r;
            }
            const std::wstring into = temp.path() + L"\\x";
            if (!CreateDirectoryW(into.c_str(), nullptr)) {
                r.message = L"A temporary folder could not be created: " + WinError(GetLastError());
                return r;
            }
            if (progress) progress(std::format(L"Extracting {} files ({})", pick.size(), Bytes(total)));
            if (!ExtractWithTar(paths[0], pick, into, temp.path() + L"\\tar.txt", error)) {
                r.message = error;
                return r;
            }
        }
        for (const Entry* e : pick) {
            if (SameName(BaseName(e->name), kIntelWeights) && !CheckSafetensors(e->source, error)) {
                r.message = e->name + L" " + error + L". Nothing was installed.";
                return r;
            }
            copies.push_back({e->source, BaseName(e->name)});
        }
    }

    if (!Install(destination, copies, progress, error)) {
        r.message = error.empty() ? std::wstring(L"The files could not be installed.") : error;
        if (r.message.find(L"Nothing was installed") == std::wstring::npos) r.message += L" The previous files were kept.";
        return r;
    }
    r.installed = true;
    for (const Copy& c : copies) r.files.push_back(c.name);
    MSR_LOG_INFO(L"Neural runtime import into {}: {}", destination, JoinFiles(r.files));

    // Validation of what is now installed.
    if (progress) progress(L"Checking the installed files");
    const std::wstring installedList = JoinFiles(r.files);
    if (amd) {
        const std::wstring dll = destination + L"\\" + kAmdDll;
        PeInfo pe;
        if (!InspectPe(dll, pe, error)) {
            r.message = L"Installed " + installedList + L", but LmxxfNrRuntime.dll " + error +
                        L". Choose the files from the AMDNR release (AMDNR-v0.3.5.6.zip).";
            return r;
        }
        if (!HasExport(pe, "LmxxfNrGetApi")) {
            r.message = L"Installed " + installedList + L", but this LmxxfNrRuntime.dll is not the AMDNR runtime (it has no "
                        L"LmxxfNrGetApi). Choose the files from the AMDNR release (AMDNR-v0.3.5.6.zip).";
            return r;
        }
        if (!ProbeLoad(dll, "LmxxfNrGetApi", error)) {
            r.message = L"Installed " + installedList + L", but the runtime does not load on this PC: " + error;
            return r;
        }
        r.validated = true;
        r.message = L"Installed " + installedList + L". The AMDNR runtime loads on this PC. Whether your GPU is supported "
                    L"is checked when scaling starts.";
        return r;
    }
    const IntelFiles f = IntelStatus(destination);
    PeInfo pe;
    if (!InspectPe(destination + L"\\" + kIntelDll, pe, error)) {
        r.message = L"Installed " + installedList + L", but d5c_chain.dll " + error + L". Use the d5c_chain.dll built from DLSS5_INTEL.";
        return r;
    }
    for (const char* name : kIntelExports)
        if (!HasExport(pe, name)) {
            r.message = L"Installed " + installedList + L", but this d5c_chain.dll does not export " +
                        std::wstring(name, name + strlen(name)) + L", which MoonSR needs (reference commit 7e8e039).";
            return r;
        }
    if (!f.chain || !f.shaders) {
        r.message = L"Installed " + installedList + L", but intel-nr is still missing d5c_chain.dll or gemm.spv.";
        return r;
    }
    r.validated = true;
    r.message = f.weights ? L"Installed " + installedList + L". The Intel runtime, its shaders and the weights are in place."
                          : L"Installed " + installedList + L". Next, import your weights (dlssnr-logical.safetensors).";
    return r;
}

bool CheckIntelChain(const std::wstring& dll, std::wstring& error) {
    PeInfo pe;
    if (!InspectPe(dll, pe, error)) {
        error = L"d5c_chain.dll " + error;
        return false;
    }
    for (const char* name : kIntelExports)
        if (!HasExport(pe, name)) {
            error = L"d5c_chain.dll does not export " + std::wstring(name, name + strlen(name));
            return false;
        }
    return true;
}

}  // namespace moonsr::app::nr
