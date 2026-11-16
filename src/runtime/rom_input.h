#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gbarecomp {
// Archive decoding is an optional desktop dependency. Paths remain the user's
// original paths: this function never extracts files or changes save/cache roots.
bool rom_archives_available();
bool read_rom_input(const std::string& path, const char* expected_sha1,
                    std::vector<std::uint8_t>* bytes, std::string* error);
} // namespace gbarecomp
