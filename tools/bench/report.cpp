#include "report.h"

#include "gpu_runner.h"

#include <windows.h>

#include <cmath>
#include <ctime>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace bench {

std::string Num(double v, int decimals) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    char b[64];
    snprintf(b, sizeof(b), "%.*f", decimals, v);
    return b;
}

fs::path FindRepoRoot() {
    auto isRoot = [](const fs::path& p) { return fs::exists(p / "CLAUDE.md") && fs::exists(p / "shaders") && fs::exists(p / "src"); };
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    for (fs::path p = fs::path(buf).parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path())
        if (isRoot(p)) return p;
    for (fs::path p = fs::current_path(); !p.empty() && p != p.parent_path(); p = p.parent_path())
        if (isRoot(p)) return p;
    return fs::current_path();
}

namespace {
std::string RegString(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[256];
    DWORD sz = sizeof(buf), type = 0;
    std::string r;
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz) == ERROR_SUCCESS && type == REG_SZ) r = Narrow(buf);
    RegCloseKey(k);
    return r;
}
DWORD RegDword(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY k;
    DWORD v = 0, sz = sizeof(v);
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    if (RegQueryValueExW(k, name, nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &sz) != ERROR_SUCCESS) v = 0;
    RegCloseKey(k);
    return v;
}

std::string RunCapture(const std::string& cmd) {
    std::string out;
    FILE* p = _popen(cmd.c_str(), "r");
    if (!p) return out;
    char b[256];
    while (fgets(b, sizeof(b), p)) out += b;
    _pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

// FNV-1a over path + content of the sources that determine what the scaler does and how it is run. Identifies the code
// state when the tree has no commit. It is computed from the files on disk at run time, so it only matches the
// executable if the executable was rebuilt after the last edit.
std::string HashSources(const fs::path& root) {
    std::vector<fs::path> files;
    for (const char* d : {"shaders", "src/core", "src/gpu", "src/upscale"}) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root / d, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            const auto e = it->path().extension().string();
            if (e == ".hlsl" || e == ".hlsli" || e == ".cpp" || e == ".h" || it->path().filename() == "CMakeLists.txt") files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    uint64_t h = 1469598103934665603ull;
    for (const auto& f : files) {
        const std::string rel = fs::relative(f, root).generic_string();
        h = Fnv1a(rel.data(), rel.size(), h);
        std::ifstream in(f, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        h = Fnv1a(data.data(), data.size(), h);
    }
    char b[32];
    snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(h));
    return std::string(b) + " (" + std::to_string(files.size()) + " files)";
}
} // namespace

SystemInfo CollectSystemInfo(const fs::path& root, const std::string& commandLine) {
    SystemInfo s;
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
        fn(&vi);
    const DWORD ubr = RegDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR");
    s.osBuild = std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion) + "." + std::to_string(vi.dwBuildNumber) + "." + std::to_string(ubr);
    s.cpu = RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    while (!s.cpu.empty() && s.cpu.back() == ' ') s.cpu.pop_back();
    std::string git = RunCapture("git -C \"" + root.string() + "\" rev-parse --short HEAD 2>nul");
    s.gitCommit = (git.empty() || git.find(' ') != std::string::npos || git.size() > 40) ? "uncommitted" : git;
    s.sourceHash = HashSources(root);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[64];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    s.date = b;
    s.commandLine = commandLine;
    return s;
}

Record AdapterFields(const moonsr::AdapterInfo& a) {
    char ids[32];
    snprintf(ids, sizeof(ids), "0x%04X:0x%04X", a.vendorId, a.deviceId);
    return {S("adapter", Narrow(a.name)), S("driver_umd", DriverVersionString(a.driverVersion)), S("vendor_device_id", ids)};
}

Record RunFields(const SystemInfo& s) {
    return {S("os_build", s.osBuild), S("git_commit", s.gitCommit), S("source_hash", s.sourceHash), S("date", s.date)};
}

namespace {
std::string CsvEsc(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}
std::string JsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o;
}
} // namespace

bool WriteCsv(const fs::path& path, const std::vector<Record>& rows, std::string& err) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path.string(); return false; }
    // Union of keys in first-seen order.
    std::vector<std::string> cols;
    for (const auto& r : rows)
        for (const auto& fld : r)
            if (std::find(cols.begin(), cols.end(), fld.key) == cols.end()) cols.push_back(fld.key);
    for (size_t i = 0; i < cols.size(); ++i) f << (i ? "," : "") << cols[i];
    f << "\n";
    for (const auto& r : rows) {
        for (size_t i = 0; i < cols.size(); ++i) {
            std::string v;
            for (const auto& fld : r) if (fld.key == cols[i]) { v = fld.val; break; }
            f << (i ? "," : "") << CsvEsc(v);
        }
        f << "\n";
    }
    return true;
}

bool WriteJson(const fs::path& path, const Record& meta, const std::vector<Record>& rows, std::string& err) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path.string(); return false; }
    auto obj = [&](const Record& r, const char* indent) {
        f << "{";
        for (size_t i = 0; i < r.size(); ++i) {
            f << (i ? ", " : "") << "\"" << r[i].key << "\": ";
            if (r[i].numeric) f << r[i].val;
            else f << "\"" << JsonEsc(r[i].val) << "\"";
        }
        f << "}";
        (void)indent;
    };
    f << "{\n  \"meta\": ";
    obj(meta, "  ");
    f << ",\n  \"records\": [\n";
    for (size_t i = 0; i < rows.size(); ++i) {
        f << "    ";
        obj(rows[i], "    ");
        f << (i + 1 < rows.size() ? ",\n" : "\n");
    }
    f << "  ]\n}\n";
    return true;
}

void PrintTable(const std::vector<Record>& rows, const std::vector<std::string>& cols) {
    std::vector<size_t> width(cols.size());
    std::vector<bool> numeric(cols.size(), true);
    auto get = [&](const Record& r, const std::string& k, bool* num) -> std::string {
        for (const auto& f : r) if (f.key == k) { if (num) *num = f.numeric || f.val == "inf"; return f.val; }
        return "";
    };
    for (size_t c = 0; c < cols.size(); ++c) {
        width[c] = cols[c].size();
        for (const auto& r : rows) {
            bool n = false;
            const std::string v = get(r, cols[c], &n);
            width[c] = std::max(width[c], v.size());
            if (!n) numeric[c] = false;
        }
    }
    auto line = [&]() {
        for (size_t c = 0; c < cols.size(); ++c) printf("%s%s", c ? "-+-" : "", std::string(width[c], '-').c_str());
        printf("\n");
    };
    for (size_t c = 0; c < cols.size(); ++c) printf("%s%-*s", c ? " | " : "", static_cast<int>(width[c]), cols[c].c_str());
    printf("\n");
    line();
    for (const auto& r : rows) {
        for (size_t c = 0; c < cols.size(); ++c) {
            const std::string v = get(r, cols[c], nullptr);
            if (numeric[c]) printf("%s%*s", c ? " | " : "", static_cast<int>(width[c]), v.c_str());
            else printf("%s%-*s", c ? " | " : "", static_cast<int>(width[c]), v.c_str());
        }
        printf("\n");
    }
}

} // namespace bench
