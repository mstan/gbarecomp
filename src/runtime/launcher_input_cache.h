#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace gbarecomp_seam {

inline std::string read_single_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (f && std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                 line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        return line;
    }
    return {};
}

inline void write_single_line(const std::string& path, const std::string& value) {
    if (value.empty()) return;
    std::string stored = value;
    // The launcher receives an absolute seed even when the player deliberately
    // saved a relative input path. Keep that spelling on an unchanged commit,
    // so the next launch can still relocate with the package.
    try {
        const std::string previous = read_single_line(path);
        const std::filesystem::path old(previous);
        if (!previous.empty() && old.is_relative()) {
            const auto base = std::filesystem::path(path).parent_path();
            auto selected = std::filesystem::path(value);
            if (selected.is_relative()) selected = base / selected;
            std::error_code ec;
            if (std::filesystem::exists(base / old, ec) && !ec) {
                const auto before = std::filesystem::weakly_canonical(base / old, ec);
                if (!ec) {
                    const auto after = std::filesystem::weakly_canonical(selected, ec);
                    if (!ec && before == after) stored = previous;
                }
            }
        }
    } catch (const std::filesystem::filesystem_error&) {
        // A path the host cannot inspect retains the original write behavior.
    }
    std::ofstream f(path, std::ios::trunc);
    if (f) f << stored << "\n";
}

} // namespace gbarecomp_seam
