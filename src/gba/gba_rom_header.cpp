// gba_rom_header.cpp — see gba_rom_header.h.

#include "gba_rom_header.h"

#include <cstring>

namespace gba {

const char* save_type_name(SaveType t) noexcept {
    switch (t) {
        case SaveType::Unknown:  return "unknown";
        case SaveType::SRAM:     return "SRAM";
        case SaveType::Flash512: return "Flash 64KB (512 Kbit)";
        case SaveType::Flash1M:  return "Flash 128KB (1 Mbit)";
        case SaveType::EEPROM:   return "EEPROM";
    }
    return "?";
}

namespace {

uint32_t read32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Try to interpret `word` as an unconditional ARM B/BL at `pc`.
// Returns true on success and sets `*target`.
bool decode_entry_branch(uint32_t word, uint32_t pc, uint32_t* target) {
    // Condition must be AL (0xE) and opcode bits 27..25 must be 101.
    uint32_t cond = (word >> 28) & 0xFu;
    uint32_t op   = (word >> 25) & 0x7u;
    if (cond != 0xE || op != 0b101) return false;
    int32_t imm24 = static_cast<int32_t>(word & 0x00FFFFFFu);
    if (imm24 & 0x00800000) imm24 |= 0xFF000000;
    *target = static_cast<uint32_t>(static_cast<int32_t>(pc + 8) + (imm24 << 2));
    return true;
}

// The Nintendo logo is a fixed 156-byte sequence at 0x004..0x09F that
// the BIOS checks before booting. A *real* hardware-equivalent check
// would byte-compare against the canonical pattern, but the canonical
// bytes are copyrighted and we don't bundle them. For Phase 0 we
// instead apply a cheap structural check: the logo region is dense
// (high byte-value variance) and non-zero — a fully-zeroed or
// fully-0xFF region means the logo is missing.
bool looks_like_logo(const uint8_t* p) {
    int zeros = 0;
    int ffs   = 0;
    for (int i = 0; i < 156; ++i) {
        if (p[i] == 0x00) ++zeros;
        if (p[i] == 0xFF) ++ffs;
    }
    if (zeros > 140) return false;
    if (ffs > 140) return false;
    return true;
}

struct SaveSignature {
    const char* str;
    SaveType    type;
};

// Order matters: longer / more-specific signatures must come before
// their prefixes. "FLASH1M_V" / "FLASH512_V" before "FLASH_V"; the
// match logic returns the first signature seen.
constexpr SaveSignature kSaveSignatures[] = {
    { "EEPROM_V",   SaveType::EEPROM   },
    { "SRAM_V",     SaveType::SRAM     },
    { "FLASH1M_V",  SaveType::Flash1M  },
    { "FLASH512_V", SaveType::Flash512 },
    { "FLASH_V",    SaveType::Flash512 },   // bare FLASH_V is 512Kbit
};

}  // namespace

SaveType detect_save_type(const uint8_t* rom, std::size_t rom_size,
                          uint32_t* offset_out,
                          std::string* matched_out) {
    if (!rom || rom_size < 16) return SaveType::Unknown;
    // Save signatures are aligned to 4-byte boundaries in real ROMs.
    // We scan with that alignment to keep the scan cheap. (A signature
    // off-alignment is a malformed ROM; we don't try to recover.)
    for (std::size_t off = 0; off + 16 < rom_size; off += 4) {
        for (const auto& sig : kSaveSignatures) {
            std::size_t n = std::strlen(sig.str);
            if (off + n >= rom_size) continue;
            if (std::memcmp(rom + off, sig.str, n) == 0) {
                if (offset_out)  *offset_out = static_cast<uint32_t>(off);
                if (matched_out) *matched_out = sig.str;
                return sig.type;
            }
        }
    }
    return SaveType::Unknown;
}

RomHeader parse_rom(const uint8_t* rom, std::size_t rom_size) {
    RomHeader h;
    if (!rom || rom_size < 0xC0) {
        h.error = "rom smaller than header (need at least 0xC0 bytes)";
        return h;
    }

    h.entry_branch_word = read32_le(rom + 0x00);
    h.entry_is_branch = decode_entry_branch(h.entry_branch_word,
                                            0x08000000u,
                                            &h.entry_target);

    h.logo_present = looks_like_logo(rom + 0x04);

    // Game title: ASCII, up to 12 bytes, NUL-padded.
    {
        char buf[13]{};
        std::memcpy(buf, rom + 0xA0, 12);
        // Trim trailing NUL/space.
        int n = 12;
        while (n > 0 && (buf[n - 1] == '\0' || buf[n - 1] == ' ')) --n;
        h.game_title.assign(buf, buf + n);
    }
    {
        char buf[5]{};
        std::memcpy(buf, rom + 0xAC, 4);
        h.game_code.assign(buf, buf + 4);
    }
    {
        char buf[3]{};
        std::memcpy(buf, rom + 0xB0, 2);
        h.maker_code.assign(buf, buf + 2);
    }
    h.fixed_b2         = rom[0xB2];
    h.main_unit_code   = rom[0xB3];
    h.software_version = rom[0xBC];
    h.complement_check = rom[0xBD];

    // Complement = -(0x19 + sum(rom[0xA0..0xBC])) & 0xFF.
    uint32_t sum = 0x19;
    for (int off = 0xA0; off <= 0xBC; ++off) sum += rom[off];
    h.complement_expected = static_cast<uint8_t>((-static_cast<int>(sum)) & 0xFF);
    h.complement_valid = (h.complement_expected == h.complement_check);

    // Save chip scan (full ROM).
    h.save_type = detect_save_type(rom, rom_size,
                                   &h.save_signature_offset,
                                   &h.save_signature);

    // Overall ok: header structure parsed, fixed bytes match.
    h.ok = h.entry_is_branch &&
           h.fixed_b2 == 0x96 &&
           h.main_unit_code == 0x00;
    if (!h.ok) {
        if (!h.entry_is_branch)        h.error = "entry word is not an ARM B/BL";
        else if (h.fixed_b2 != 0x96)   h.error = "fixed byte 0xB2 != 0x96";
        else if (h.main_unit_code != 0) h.error = "main unit code != 0";
    }
    return h;
}

}  // namespace gba
