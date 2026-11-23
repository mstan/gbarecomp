#pragma once

#include <fstream>
#include <optional>
#include <string>

namespace gbarecomp {

// The old player choice is imported by the visible Mods control. Keep one
// reader for that migration and for runners without a diagnostics catalog.
inline std::optional<bool> diagnostic_replay_saved_setting(
        const std::string& config_ini) {
    std::ifstream in(config_ini);
    std::optional<bool> value;
    std::string line;
    bool section = false;
    auto trim = [](std::string v) {
        const auto b = v.find_first_not_of(" \t\r");
        const auto e = v.find_last_not_of(" \t\r");
        return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
    };
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[') {
            section = trim(line.substr(1, line.find(']') - 1)) == "Diagnostics";
            continue;
        }
        const auto eq = line.find('=');
        if (section && eq != std::string::npos &&
            trim(line.substr(0, eq)) == "replay_capture")
            value = trim(line.substr(eq + 1)) != "0";
    }
    return value;
}

}  // namespace gbarecomp
