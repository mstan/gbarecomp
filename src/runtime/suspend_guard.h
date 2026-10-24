#pragma once

#include "../gba/sha1.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace gbarecomp {
inline std::string suspend_battery_fingerprint(const std::string& path) {
    if (path.empty()) return "disabled";
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) return {};
    if (!exists) return "missing";
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > 16u * 1024u * 1024u) return {};
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return {};
    return gba::sha1(bytes.data(), bytes.size()).hex();
}

inline bool write_suspend_marker(const std::string& marker, const std::string& save,
                                 std::uint64_t frame) {
    const auto fingerprint = suspend_battery_fingerprint(save);
    if (fingerprint.empty()) return false;
    std::ofstream file(marker, std::ios::trunc);
    file << frame << '\n' << fingerprint << '\n';
    file.flush();
    return static_cast<bool>(file);
}

inline bool suspend_marker_matches_battery(const std::string& marker,
                                           const std::string& save) {
    std::ifstream file(marker);
    std::uint64_t frame = 0;
    std::string recorded;
    if (!(file >> frame >> recorded)) return false;
    const auto fingerprint = suspend_battery_fingerprint(save);
    return !fingerprint.empty() && recorded == fingerprint;
}
} // namespace gbarecomp
