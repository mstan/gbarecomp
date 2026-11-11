#pragma once

#include <fstream>
#include <string>
#include <vector>

namespace gbarecomp {

// Update only the three presentation choices. Preserve controller bindings,
// save paths and every other launcher's setting in the same INI file.
inline void save_presentation_settings(const std::string& path, bool integer_scale,
                                       bool scanlines, int strength) {
    if (path.empty()) return;
    std::vector<std::string> lines;
    std::ifstream input(path);
    std::string line;
    bool launcher = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto start = line.find_first_not_of(" \t");
        if (start != std::string::npos && line[start] == '[')
            launcher = line.compare(start, 10, "[Launcher]") == 0;
        if (launcher && start != std::string::npos) {
            const auto eq = line.find('=', start);
            if (eq != std::string::npos) {
                const auto end = line.find_last_not_of(" \t", eq - 1);
                const auto key = line.substr(start, end - start + 1);
                if (key == "integer_scale" || key == "scanlines" || key == "scanline_strength") continue;
            }
        }
        lines.push_back(line);
    }
    input.close();
    std::ofstream output(path, std::ios::trunc);
    if (!output) return;
    bool inserted = false;
    for (const auto& kept : lines) {
        output << kept << '\n';
        const auto start = kept.find_first_not_of(" \t");
        if (!inserted && start != std::string::npos && kept.compare(start, 10, "[Launcher]") == 0) {
            output << "integer_scale = " << integer_scale << "\nscanlines = " << scanlines
                   << "\nscanline_strength = " << strength << '\n';
            inserted = true;
        }
    }
    if (!inserted)
        output << "\n[Launcher]\ninteger_scale = " << integer_scale << "\nscanlines = " << scanlines
               << "\nscanline_strength = " << strength << '\n';
}

} // namespace gbarecomp
