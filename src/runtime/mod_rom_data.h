#pragma once
#include "runtime.h"
#include <string>
#include <vector>

namespace gbarecomp::mod_rom_data_internal {
// The runtime calls this before publishing its process-local image to any
// device. Failure must stop boot; it does not publish a partial image.
bool prepare(const RunOptions& options, std::vector<std::uint8_t>& image,
             std::string* effective_sha1, std::string* error);
}
