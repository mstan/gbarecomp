// bus_tests — current scope: ROM header parser. As more bus
// subsystems come online (Phase 2/3) this file expands to cover them.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gba_rom_header.h"
#include "gba_save.h"

namespace {

int failures = 0;

template <typename A, typename B>
void check_eq(const char* test, const char* tag, A got, B expect) {
    if (got != static_cast<A>(expect)) {
        std::printf("FAIL %s: %s mismatch (got 0x%llx, expected 0x%llx)\n",
                    test, tag,
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(expect));
        ++failures;
    }
}

void check_str(const char* test, const char* tag,
               const std::string& got, const char* expect) {
    if (got != expect) {
        std::printf("FAIL %s: %s = \"%s\" (expected \"%s\")\n",
                    test, tag, got.c_str(), expect);
        ++failures;
    }
}

void check_bool(const char* test, const char* tag,
                bool got, bool expect) {
    if (got != expect) {
        std::printf("FAIL %s: %s = %d (expected %d)\n",
                    test, tag, got ? 1 : 0, expect ? 1 : 0);
        ++failures;
    }
}

// Build a synthetic but valid-shape ROM and verify the parser
// extracts each field correctly.
std::vector<uint8_t> build_synthetic_rom(const char* title,
                                         const char* code,
                                         const char* maker,
                                         uint8_t software_version,
                                         const char* save_signature,
                                         std::size_t total_size) {
    std::vector<uint8_t> rom(total_size, 0);

    // 0x000: B 0x080000C0  (jump to immediately after header).
    //   imm24 such that 0x080000C0 = (pc=0x08000000) + 8 + (imm24 << 2)
    //   → imm24 = (0xC0 - 8) / 4 = 0x2E.
    uint32_t branch = 0xEA00002Eu;
    rom[0x00] = branch        & 0xFF;
    rom[0x01] = (branch >> 8)  & 0xFF;
    rom[0x02] = (branch >> 16) & 0xFF;
    rom[0x03] = (branch >> 24) & 0xFF;

    // 0x004..0x09F: logo — we just fill with non-zero, non-0xFF varied
    // bytes so looks_like_logo() accepts it.
    for (int i = 0; i < 156; ++i) {
        rom[0x04 + i] = static_cast<uint8_t>((i * 17 + 3) & 0xFF);
    }

    // 0x0A0..0x0AB: title (NUL-padded).
    std::memset(rom.data() + 0xA0, 0, 12);
    std::memcpy(rom.data() + 0xA0, title,
                std::min<std::size_t>(12, std::strlen(title)));

    // 0x0AC..0x0AF: game code.
    std::memcpy(rom.data() + 0xAC, code, 4);

    // 0x0B0..0x0B1: maker.
    std::memcpy(rom.data() + 0xB0, maker, 2);

    rom[0xB2] = 0x96;   // fixed
    rom[0xB3] = 0x00;   // GBA main unit
    rom[0xB4] = 0x00;   // device type
    // 0xB5..0xBB: reserved zero.
    rom[0xBC] = software_version;

    // 0xBD: complement. Computed as -(0x19 + sum(0xA0..0xBC)) & 0xFF.
    uint32_t sum = 0x19;
    for (int off = 0xA0; off <= 0xBC; ++off) sum += rom[off];
    rom[0xBD] = static_cast<uint8_t>((-static_cast<int>(sum)) & 0xFF);

    // Drop the save signature at 0x1000 (4-byte aligned) if requested.
    if (save_signature && total_size >= 0x1100) {
        std::memcpy(rom.data() + 0x1000, save_signature,
                    std::strlen(save_signature));
    }

    return rom;
}

void test_minishcap_like() {
    auto rom = build_synthetic_rom("ZELDA MC", "AZME", "01", 0x00,
                                   "EEPROM_V124", 64 * 1024);
    auto h = gba::parse_rom(rom.data(), rom.size());
    check_bool("minishcap_like", "ok", h.ok, true);
    check_str ("minishcap_like", "title", h.game_title, "ZELDA MC");
    check_str ("minishcap_like", "code",  h.game_code,  "AZME");
    check_str ("minishcap_like", "maker", h.maker_code, "01");
    check_eq  ("minishcap_like", "fixed_b2", h.fixed_b2, 0x96);
    check_eq  ("minishcap_like", "entry_target", h.entry_target, 0x080000C0u);
    check_bool("minishcap_like", "entry_is_branch", h.entry_is_branch, true);
    check_bool("minishcap_like", "complement_valid", h.complement_valid, true);
    check_bool("minishcap_like", "logo_present", h.logo_present, true);
    check_eq  ("minishcap_like", "save_type",
               static_cast<int>(h.save_type), static_cast<int>(gba::SaveType::EEPROM));
    check_str ("minishcap_like", "save_signature", h.save_signature, "EEPROM_V");
    check_eq  ("minishcap_like", "save_signature_offset",
               h.save_signature_offset, 0x1000u);
}

void test_corrupt_header() {
    auto rom = build_synthetic_rom("X", "XXXX", "00", 0x00,
                                   nullptr, 64 * 1024);
    // Stomp the fixed 0x96 byte.
    rom[0xB2] = 0x00;
    auto h = gba::parse_rom(rom.data(), rom.size());
    check_bool("corrupt_header", "ok", h.ok, false);
}

void test_no_save_signature() {
    auto rom = build_synthetic_rom("GAME", "XXXX", "00", 0x00,
                                   nullptr, 64 * 1024);
    auto h = gba::parse_rom(rom.data(), rom.size());
    check_bool("no_save_signature", "ok", h.ok, true);
    check_eq  ("no_save_signature", "save_type",
               static_cast<int>(h.save_type),
               static_cast<int>(gba::SaveType::Unknown));
}

void test_sram_signature() {
    auto rom = build_synthetic_rom("GAME", "XXXX", "00", 0x00,
                                   "SRAM_V112", 64 * 1024);
    auto h = gba::parse_rom(rom.data(), rom.size());
    check_eq  ("sram_signature", "save_type",
               static_cast<int>(h.save_type),
               static_cast<int>(gba::SaveType::SRAM));
}

void test_flash1m_beats_flash() {
    // FLASH1M_V must win over the bare FLASH_V prefix detection.
    auto rom = build_synthetic_rom("GAME", "XXXX", "00", 0x00,
                                   "FLASH1M_V103", 64 * 1024);
    auto h = gba::parse_rom(rom.data(), rom.size());
    check_eq  ("flash1m_beats_flash", "save_type",
               static_cast<int>(h.save_type),
               static_cast<int>(gba::SaveType::Flash1M));
}

void eeprom_send_bits(gba::GbaSave& save, uint32_t value, int count) {
    for (int bit = count - 1; bit >= 0; --bit) {
        save.eeprom_write_bit(static_cast<uint16_t>((value >> bit) & 1u));
    }
}

void eeprom_send_read(gba::GbaSave& save, uint32_t block) {
    eeprom_send_bits(save, 0b11, 2);
    eeprom_send_bits(save, block, 14);
    eeprom_send_bits(save, 0, 1);
}

void eeprom_send_write(gba::GbaSave& save, uint32_t block,
                       const uint8_t bytes[8]) {
    eeprom_send_bits(save, 0b10, 2);
    eeprom_send_bits(save, block, 14);
    for (int i = 0; i < 8; ++i) {
        eeprom_send_bits(save, bytes[i], 8);
    }
    eeprom_send_bits(save, 0, 1);
}

void test_eeprom_8k_read_write() {
    gba::GbaSave save;
    save.configure_eeprom(8 * 1024);

    eeprom_send_read(save, 3);
    for (int i = 0; i < 4; ++i) {
        check_eq("eeprom_8k", "blank_dummy", save.eeprom_read_bit(), 0u);
    }
    for (int i = 0; i < 64; ++i) {
        check_eq("eeprom_8k", "blank_data", save.eeprom_read_bit(), 1u);
    }

    const uint8_t pattern[8] = {0x12, 0x34, 0x56, 0x78,
                                0x9a, 0xbc, 0xde, 0xf0};
    eeprom_send_write(save, 3, pattern);
    eeprom_send_read(save, 3);
    for (int i = 0; i < 4; ++i) {
        (void)save.eeprom_read_bit();
    }
    for (int i = 0; i < 8; ++i) {
        uint8_t got = 0;
        for (int bit = 0; bit < 8; ++bit) {
            got = static_cast<uint8_t>((got << 1) | save.eeprom_read_bit());
        }
        check_eq("eeprom_8k", "written_byte", got, pattern[i]);
    }
}

}  // namespace

int main() {
    test_minishcap_like();
    test_corrupt_header();
    test_no_save_signature();
    test_sram_signature();
    test_flash1m_beats_flash();
    test_eeprom_8k_read_write();
    if (failures) {
        std::printf("\n%d failure(s)\n", failures);
        return 1;
    }
    std::printf("bus_tests (rom header): OK\n");
    return 0;
}
