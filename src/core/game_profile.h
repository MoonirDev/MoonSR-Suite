#pragma once
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

namespace moonsr {
// Both adaptive systems use this directory for the same canonical executable path.
// Their independently versioned data remain separate files inside it.
inline std::wstring GameProfileDirectory(const std::wstring& root, const std::wstring& executable) {
    if (root.empty() || executable.empty()) return {};
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : executable) {
        const uint16_t value = static_cast<uint16_t>(c);
        hash ^= value & 0xff; hash *= 1099511628211ULL;
        hash ^= value >> 8; hash *= 1099511628211ULL;
    }
    std::wostringstream name;
    name << L"game-" << std::hex << std::setw(16) << std::setfill(L'0') << hash;
    return (std::filesystem::path(root) / name.str()).wstring();
}
}
