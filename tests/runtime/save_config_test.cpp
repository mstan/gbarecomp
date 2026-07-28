#include "save_config.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "save_config_tests: %s\n", message);
    std::exit(1);
}

gbarecomp::SaveConfiguration resolve(
    gba::SaveType detected,
    std::optional<gba::SaveType> configured = std::nullopt,
    std::size_t size = 0,
    const char* environment = nullptr) {
    gbarecomp::SaveConfiguration result;
    std::string error;
    check(gbarecomp::resolve_save_configuration(
              detected, configured, size, environment, &result, &error),
          error.c_str());
    return result;
}

}  // namespace

int main() {
    gba::SaveType parsed = gba::SaveType::Unknown;
    check(gbarecomp::parse_save_type("sram", &parsed) &&
              parsed == gba::SaveType::SRAM,
          "failed to parse sram");
    check(gbarecomp::parse_save_type("FLASH1M", &parsed) &&
              parsed == gba::SaveType::Flash1M,
          "save type parsing should be case-insensitive");
    check(!gbarecomp::parse_save_type("flash", &parsed),
          "ambiguous flash type was accepted");

    auto result = resolve(gba::SaveType::EEPROM);
    check(result.type == gba::SaveType::EEPROM &&
              result.size == 8192 &&
              result.source == gbarecomp::SaveTypeSource::Detected,
          "ROM detection did not select default EEPROM geometry");

    result = resolve(gba::SaveType::EEPROM, gba::SaveType::SRAM);
    check(result.type == gba::SaveType::SRAM &&
              result.size == 32768 &&
              result.source == gbarecomp::SaveTypeSource::Config,
          "game config did not override ROM detection");

    result = resolve(gba::SaveType::EEPROM, gba::SaveType::SRAM, 0,
                     "flash1m");
    check(result.type == gba::SaveType::Flash1M &&
              result.size == 131072 &&
              result.source == gbarecomp::SaveTypeSource::Environment,
          "environment did not override game config");

    result = resolve(gba::SaveType::EEPROM, gba::SaveType::EEPROM, 512);
    check(result.size == 512, "valid 512-byte EEPROM was rejected");

    gbarecomp::SaveConfiguration rejected;
    std::string error;
    check(!gbarecomp::resolve_save_configuration(
              gba::SaveType::EEPROM, gba::SaveType::EEPROM, 8192,
              "sram", &rejected, &error) &&
              error.find("incompatible") != std::string::npos,
          "stale EEPROM size was accepted after forcing SRAM");
    check(!gbarecomp::resolve_save_configuration(
              gba::SaveType::SRAM, std::nullopt, 0, "not-a-chip",
              &rejected, &error) &&
              error.find("unknown GBARECOMP_SAVE_TYPE") != std::string::npos,
          "unknown environment override was accepted");

    std::puts("save_config_tests: all cases passed");
    return 0;
}
