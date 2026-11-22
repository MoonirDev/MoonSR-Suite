// Thread-safe logging. Writes to OutputDebugString and, once InitLog is called, to a log file.
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace moonsr {

enum class LogLevel { Debug, Info, Warn, Error };

// Opens (truncates) the log file. Safe to call once at startup; logging before this goes to the debugger only.
void InitLog(const std::wstring& filePath);
void LogWrite(LogLevel level, std::wstring_view message);

// Formats an HRESULT as "0x8XXXXXXX (system message)".
std::wstring HrToString(long hr);

template <class... Args>
void Log(LogLevel level, std::wformat_string<Args...> fmt, Args&&... args) {
    LogWrite(level, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace moonsr

#define MSR_LOG_DEBUG(...) ::moonsr::Log(::moonsr::LogLevel::Debug, __VA_ARGS__)
#define MSR_LOG_INFO(...) ::moonsr::Log(::moonsr::LogLevel::Info, __VA_ARGS__)
#define MSR_LOG_WARN(...) ::moonsr::Log(::moonsr::LogLevel::Warn, __VA_ARGS__)
#define MSR_LOG_ERROR(...) ::moonsr::Log(::moonsr::LogLevel::Error, __VA_ARGS__)
