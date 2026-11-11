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
    // Inputs inside the package travel with it, including a first Browse
    // selection. External inputs keep their absolute location. Retain an
    // explicit relative spelling when it still names the same selected file.
    try {
        const std::string previous = read_single_line(path);
        const std::filesystem::path old(previous);
        const auto base = std::filesystem::weakly_canonical(
            std::filesystem::absolute(std::filesystem::path(path).parent_path()));
        auto selected = std::filesystem::path(value);
        if (selected.is_relative()) selected = base / selected;
        std::error_code ec;
        bool preserved = false;
        if (!previous.empty() && old.is_relative()) {
            preserved = std::filesystem::equivalent(base / old, selected, ec) && !ec;
            if (preserved) stored = previous;
        }
        if (!preserved) {
            ec.clear();
            const auto canonical = std::filesystem::weakly_canonical(selected, ec);
            const auto relative = ec ? std::filesystem::path{} : canonical.lexically_relative(base);
            const bool contained = !relative.empty() && relative.is_relative() &&
                *relative.begin() != "..";
            stored = contained ? relative.generic_string() : selected.lexically_normal().string();
        }
    } catch (const std::filesystem::filesystem_error&) {
        // A path the host cannot inspect retains the original write behavior.
    }
    std::ofstream f(path, std::ios::trunc);
    if (f) f << stored << "\n";
}

} // namespace gbarecomp_seam
