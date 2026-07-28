#include "save_config.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace gbarecomp {
namespace {

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return result;
}

std::string expected_size_text(gba::SaveType type) {
    switch (type) {
        case gba::SaveType::SRAM:     return "32768";
        case gba::SaveType::EEPROM:   return "512 or 8192";
        case gba::SaveType::Flash512: return "65536";
        case gba::SaveType::Flash1M:  return "131072";
        case gba::SaveType::Unknown:  return "0";
    }
    return "0";
}

}  // namespace

bool parse_save_type(std::string_view value, gba::SaveType* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "sram") {
        *out = gba::SaveType::SRAM;
    } else if (normalized == "eeprom") {
        *out = gba::SaveType::EEPROM;
    } else if (normalized == "flash512") {
        *out = gba::SaveType::Flash512;
    } else if (normalized == "flash1m") {
        *out = gba::SaveType::Flash1M;
    } else {
        return false;
    }
    return true;
}

std::size_t default_save_size(gba::SaveType type) {
    switch (type) {
        case gba::SaveType::SRAM:     return 32u * 1024u;
        case gba::SaveType::EEPROM:   return 8u * 1024u;
        case gba::SaveType::Flash512: return 64u * 1024u;
        case gba::SaveType::Flash1M:  return 128u * 1024u;
        case gba::SaveType::Unknown:  return 0;
    }
    return 0;
}

bool valid_save_size(gba::SaveType type, std::size_t size) {
    switch (type) {
        case gba::SaveType::SRAM:
            return size == 32u * 1024u;
        case gba::SaveType::EEPROM:
            return size == 512u || size == 8u * 1024u;
        case gba::SaveType::Flash512:
            return size == 64u * 1024u;
        case gba::SaveType::Flash1M:
            return size == 128u * 1024u;
        case gba::SaveType::Unknown:
            return size == 0;
    }
    return false;
}

const char* save_type_source_name(SaveTypeSource source) noexcept {
    switch (source) {
        case SaveTypeSource::Detected:    return "rom-signature";
        case SaveTypeSource::Config:      return "game-config";
        case SaveTypeSource::Environment: return "environment";
    }
    return "unknown";
}

bool resolve_save_configuration(
    gba::SaveType detected_type,
    std::optional<gba::SaveType> configured_type,
    std::size_t configured_size,
    const char* environment_override,
    SaveConfiguration* out,
    std::string* error) {
    if (!out) {
        if (error) *error = "save configuration output is null";
        return false;
    }

    SaveConfiguration resolved;
    resolved.type = detected_type;
    if (configured_type) {
        resolved.type = *configured_type;
        resolved.source = SaveTypeSource::Config;
    }

    if (environment_override) {
        gba::SaveType forced = gba::SaveType::Unknown;
        if (!parse_save_type(environment_override, &forced)) {
            if (error) {
                *error = "unknown GBARECOMP_SAVE_TYPE=\"" +
                         std::string(environment_override) +
                         "\" (expected sram|eeprom|flash512|flash1m)";
            }
            return false;
        }
        resolved.type = forced;
        resolved.source = SaveTypeSource::Environment;
    }

    resolved.size = configured_size
        ? configured_size
        : default_save_size(resolved.type);
    if (!valid_save_size(resolved.type, resolved.size)) {
        if (error) {
            *error = "save size " + std::to_string(resolved.size) +
                     " is incompatible with " +
                     gba::save_type_name(resolved.type) +
                     " (expected " + expected_size_text(resolved.type) + ")";
        }
        return false;
    }

    *out = resolved;
    return true;
}

}  // namespace gbarecomp
