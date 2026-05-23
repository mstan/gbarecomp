// host_platform.h — STUB. Host-side things: file I/O, time, threads,
// window + audio device output.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gbarecomp {

// Read an entire file into memory. Returns empty vector + sets `error`
// on failure. Used for ROM and BIOS loading.
std::vector<unsigned char> read_file(const std::string& path,
                                     std::string* error = nullptr);

}  // namespace gbarecomp
