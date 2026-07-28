#pragma once

#include "gba_rom_header.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace gbarecomp {

enum class SaveTypeSource {
    Detected,
    Config,
    Environment,
};

struct SaveConfiguration {
    gba::SaveType type = gba::SaveType::Unknown;
    std::size_t size = 0;
    SaveTypeSource source = SaveTypeSource::Detected;
};

bool parse_save_type(std::string_view value, gba::SaveType* out);
std::size_t default_save_size(gba::SaveType type);
bool valid_save_size(gba::SaveType type, std::size_t size);
const char* save_type_source_name(SaveTypeSource source) noexcept;

// Resolve save settings in increasing precedence:
// ROM signature detection -> game.toml [save].type -> environment override.
// An explicit [save].size must be valid for the final selected chip type.
bool resolve_save_configuration(
    gba::SaveType detected_type,
    std::optional<gba::SaveType> configured_type,
    std::size_t configured_size,
    const char* environment_override,
    SaveConfiguration* out,
    std::string* error);

}  // namespace gbarecomp
