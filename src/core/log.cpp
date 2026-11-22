#include "core/log.h"
#include "core/types.h"

#include <cstdio>
#include <mutex>

namespace moonsr {

namespace {
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
int64_t g_logStart = 0;

const wchar_t* LevelName(LogLevel l) {
    switch (l) {
    case LogLevel::Debug: return L"DEBUG";
    case LogLevel::Info: return L"INFO ";
    case LogLevel::Warn: return L"WARN ";
    default: return L"ERROR";
    }
}
} // namespace

int64_t QpcFrequency() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

int64_t QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double QpcToMs(int64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(QpcFrequency()); }

void InitLog(const std::wstring& filePath) {
    std::lock_guard lock(g_logMutex);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    // Keep the previous run's log: a relaunch must not destroy the evidence of the run that had the problem.
    const std::wstring previous = filePath + L".prev";
    MoveFileExW(filePath.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    _wfopen_s(&g_logFile, filePath.c_str(), L"w, ccs=UTF-8");
    g_logStart = QpcNow();
}

void LogWrite(LogLevel level, std::wstring_view message) {
    std::lock_guard lock(g_logMutex);
    if (g_logStart == 0) g_logStart = QpcNow();
    const double t = QpcToMs(QpcNow() - g_logStart) / 1000.0;
    std::wstring line = std::format(L"[{:10.4f}] [{}] [t{:05}] {}\n", t, LevelName(level), GetCurrentThreadId(), message);
    OutputDebugStringW(line.c_str());
    if (g_logFile) {
        fputws(line.c_str(), g_logFile);
        fflush(g_logFile); // always: a log that loses its last lines is useless for diagnosing a user report
    }
}

std::wstring HrToString(long hr) {
    wchar_t* sys = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&sys), 0, nullptr);
    std::wstring text = std::format(L"0x{:08X}", static_cast<unsigned long>(hr));
    if (n && sys) {
        std::wstring_view sv(sys, n);
        while (!sv.empty() && (sv.back() == L'\r' || sv.back() == L'\n' || sv.back() == L' ')) sv.remove_suffix(1);
        text += L" (";
        text += sv;
        text += L")";
    }
    if (sys) LocalFree(sys);
    return text;
}

} // namespace moonsr
