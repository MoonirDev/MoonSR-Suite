#pragma once
#include "common.h"
#include "gpu/gpu_device.h"

#include <filesystem>
#include <string>
#include <vector>

namespace bench {

struct Field {
    std::string key;
    std::string val;
    bool numeric = false;
};
using Record = std::vector<Field>;

std::string Num(double v, int decimals); // "inf" / "-inf" / "nan" for non-finite values
inline Field S(std::string k, std::string v) { return {std::move(k), std::move(v), false}; }
inline Field N(std::string k, double v, int decimals) {
    const std::string s = Num(v, decimals);
    return {std::move(k), s, s.find_first_not_of("-0123456789.") == std::string::npos};
}
inline Field I(std::string k, long long v) { return {std::move(k), std::to_string(v), true}; }

struct SystemInfo {
    std::string osBuild, cpu, gitCommit, sourceHash, date, commandLine;
};
SystemInfo CollectSystemInfo(const std::filesystem::path& root, const std::string& commandLine);
std::filesystem::path FindRepoRoot();

// Fields identifying an adapter / run, to be prepended to every record.
Record AdapterFields(const moonsr::AdapterInfo& a);
Record RunFields(const SystemInfo& s);

bool WriteCsv(const std::filesystem::path& path, const std::vector<Record>& rows, std::string& err);
bool WriteJson(const std::filesystem::path& path, const Record& meta, const std::vector<Record>& rows, std::string& err);

// Console table: columns are taken from `cols` (record keys); numeric columns right-aligned.
void PrintTable(const std::vector<Record>& rows, const std::vector<std::string>& cols);

} // namespace bench
