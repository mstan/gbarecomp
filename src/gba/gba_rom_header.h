// gba_rom_header.h — GBA cartridge header parser.
//
// Layout per GBATEK § "GBA Cartridge Header":
//
//   0x000  32-bit ARM B/BL to entry point
//   0x004..0x09F  Nintendo logo (156 bytes, fixed bitmap)
//   0x0A0..0x0AB  Game title (12 ASCII bytes, uppercase)
//   0x0AC..0x0AF  Game code (4 ASCII bytes; e.g. "AZME")
//   0x0B0..0x0B1  Maker code (2 ASCII bytes)
//   0x0B2         Fixed: 0x96
//   0x0B3         Main unit code (0x00 = GBA)
//   0x0B4         Device type
//   0x0B5..0x0BB  Reserved (zero)
//   0x0BC         Software version
//   0x0BD         Complement check = (-(0x19 + sum(0xA0..0xBC))) & 0xFF
//   0x0BE..0x0BF  Reserved (zero)
//
// The save chip is detected by scanning the cartridge ROM for one of
// the documented Nintendo SDK signatures: "EEPROM_V", "SRAM_V",
// "FLASH_V", "FLASH512_V", "FLASH1M_V".

#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>

namespace gba {

enum class SaveType {
    Unknown,
    SRAM,
    Flash512,
    Flash1M,
    EEPROM,
};

const char* save_type_name(SaveType t) noexcept;

struct RomHeader {
    // Parsed fields
    uint32_t    entry_branch_word = 0;   // raw 32-bit at offset 0
    uint32_t    entry_target      = 0;   // computed branch target
    bool        entry_is_branch   = false;  // false = malformed
    std::string game_title;              // up to 12 bytes
    std::string game_code;               // 4 bytes
    std::string maker_code;              // 2 bytes
    uint8_t     fixed_b2          = 0;
    uint8_t     main_unit_code    = 0;
    uint8_t     software_version  = 0;
    uint8_t     complement_check  = 0;
    uint8_t     complement_expected = 0;  // recomputed
    bool        complement_valid  = false;
    bool        logo_present      = false; // checksum-only check, not pixel-exact

    // Detected save chip
    SaveType    save_type         = SaveType::Unknown;
    uint32_t    save_signature_offset = 0;  // offset in ROM where it was found
    std::string save_signature;             // the matched string

    // Errors / notes
    bool ok = false;             // overall: header parsed and minimum invariants satisfied
    std::string error;           // populated if !ok
};

// Parse a GBA cartridge image. `rom` must be at least 192 bytes; the
// minimum useful size is the full header (0xC0 bytes) plus some
// payload to scan for save strings.
RomHeader parse_rom(const uint8_t* rom, std::size_t rom_size);

// Scan-only helper: walk `rom` for save signatures and return the
// detected save type (or Unknown if none found). Returns the offset
// where the signature was found in `*offset_out` and the matched
// string in `*matched_out` (both optional).
SaveType detect_save_type(const uint8_t* rom, std::size_t rom_size,
                          uint32_t* offset_out = nullptr,
                          std::string* matched_out = nullptr);

}  // namespace gba
