// bus_tests — current scope: ROM header parser. As more bus
// subsystems come online (Phase 2/3) this file expands to cover them.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gba_rom_header.h"
#include "gba_bus.h"
#include "gba_save.h"
#include "gba_ppu.h"
#include "gba_simulation_state.h"
#include "mod_state.h"
#include "snapshot.h"

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

void test_sram_controller_and_snapshot() {
    gba::GbaSave save;
    save.configure_sram(32 * 1024);
    check_bool("sram_controller", "enabled", save.sram_enabled(), true);
    check_eq("sram_controller", "size", save.sram_size(),
             static_cast<std::size_t>(32 * 1024));
    check_eq("sram_controller", "blank", save.sram_read(0x1234), 0xFFu);

    save.sram_write(0x1234, 0x5A);
    check_eq("sram_controller", "written", save.sram_read(0x1234), 0x5Au);
    check_eq("sram_controller", "32k_mirror", save.sram_read(0x9234), 0x5Au);
    check_bool("sram_controller", "write_dirty", save.dirty(), true);
    save.clear_dirty();
    save.sram_write(0x9234, 0x5A);
    check_bool("sram_controller", "same_write_clean", save.dirty(), false);

    gbarecomp::debug::SnapshotWriter writer;
    save.serialize(writer);
    gba::GbaSave restored;
    gbarecomp::debug::SnapshotReader reader(writer.buffer().data(),
                                             writer.buffer().size());
    restored.deserialize(reader);
    check_bool("sram_snapshot", "reader_ok", reader.ok(), true);
    check_bool("sram_snapshot", "enabled", restored.sram_enabled(), true);
    check_eq("sram_snapshot", "size", restored.sram_size(),
             static_cast<std::size_t>(32 * 1024));
    check_eq("sram_snapshot", "byte", restored.sram_read(0x1234), 0x5Au);

    std::array<uint8_t, 3> persisted{{0x10, 0x20, 0x30}};
    check_bool("sram_persist", "load",
               restored.load_sram_bytes(persisted.data(), persisted.size()), true);
    check_eq("sram_persist", "prefix", restored.sram_read(2), 0x30u);
    check_eq("sram_persist", "fill", restored.sram_read(3), 0xFFu);
    check_bool("sram_persist", "load_clean", restored.dirty(), false);
}

struct SnapshotModValue { uint32_t value = 0; int restores = 0; };
bool snapshot_mod_save(void* user, gbarecomp::debug::SnapshotWriter& out, std::string*) {
    out.u32(static_cast<SnapshotModValue*>(user)->value);
    return true;
}
bool snapshot_mod_preflight(void*, gbarecomp::debug::SnapshotReader& in, std::string*) {
    (void)in.u32();
    return in.ok() && in.remaining() == 0;
}
void snapshot_mod_restore(void* user, gbarecomp::debug::SnapshotReader& in) {
    auto* state = static_cast<SnapshotModValue*>(user);
    state->value = in.u32();
    ++state->restores;
}

void test_snapshot_mods_preflight_before_guest_restore() {
    namespace debug = gbarecomp::debug;
    gba::GbaBus bus;
    gba::GbaPpu ppu;
    SnapshotModValue saved_value{0x1234u};
    debug::ModStateRegistry saved_catalog;
    const debug::ModStateProvider saved_provider = {
        "test.snapshot", 1, &saved_value,
        snapshot_mod_save, snapshot_mod_preflight, snapshot_mod_restore};
    std::string error;
    check_bool("snapshot_mods", "register saved", saved_catalog.register_provider(saved_provider, &error), true);
    const auto path = std::filesystem::temp_directory_path() / "gbarecomp-snapshot-mods-test.gbas";
    debug::SnapshotContext save_context;
    save_context.bus = &bus;
    save_context.ppu = &ppu;
    save_context.rom_sha1 = "0123456789012345678901234567890123456789";
    save_context.mod_state = &saved_catalog;
    bus.write8(0x02000000u, 0x11);
    check_bool("snapshot_mods", "save", debug::save_state(path.string().c_str(), save_context, &error), true);

    SnapshotModValue mismatch_value{0x7777u};
    debug::ModStateRegistry mismatch_catalog;
    const debug::ModStateProvider mismatch_provider = {
        "test.snapshot", 2, &mismatch_value,
        snapshot_mod_save, snapshot_mod_preflight, snapshot_mod_restore};
    check_bool("snapshot_mods", "register mismatch", mismatch_catalog.register_provider(mismatch_provider, &error), true);
    debug::SnapshotContext load_context = save_context;
    load_context.mod_state = &mismatch_catalog;
    bus.write8(0x02000000u, 0xAA);
    check_bool("snapshot_mods", "schema mismatch rejected", debug::load_state(path.string().c_str(), load_context, &error), false);
    check_eq("snapshot_mods", "guest untouched", bus.read8(0x02000000u), 0xAAu);
    check_eq("snapshot_mods", "provider not restored", mismatch_value.restores, 0);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void patch_snapshot_version(const std::filesystem::path& path, uint32_t version) {
    const char bytes[4] = {
        static_cast<char>(version & 0xffu),
        static_cast<char>((version >> 8) & 0xffu),
        static_cast<char>((version >> 16) & 0xffu),
        static_cast<char>((version >> 24) & 0xffu),
    };
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(4);
    file.write(bytes, sizeof(bytes));
}

void test_legacy_v1_snapshot_empty_catalog_only() {
    namespace debug = gbarecomp::debug;
    gba::GbaBus bus;
    gba::GbaPpu ppu;
    debug::ModStateRegistry empty_catalog;
    std::string error;
    const auto v2_path = std::filesystem::temp_directory_path() / "gbarecomp-legacy-v1-source.gbas";
    const auto v1_path = std::filesystem::temp_directory_path() / "gbarecomp-legacy-v1-state.gbas";
    const auto malformed_path = std::filesystem::temp_directory_path() / "gbarecomp-legacy-v1-malformed.gbas";
    std::error_code ec;
    std::filesystem::remove(v2_path, ec);
    std::filesystem::remove(v1_path, ec);
    std::filesystem::remove(malformed_path, ec);

    debug::SnapshotContext context;
    context.bus = &bus;
    context.ppu = &ppu;
    context.rom_sha1 = "0123456789012345678901234567890123456789";
    context.mod_state = &empty_catalog;
    bus.write8(0x02000000u, 0x11);
    check_bool("snapshot_legacy_v1", "write v2 source",
               debug::save_state(v2_path.string().c_str(), context, &error), true);
    std::filesystem::copy_file(v2_path, v1_path, std::filesystem::copy_options::overwrite_existing, ec);
    patch_snapshot_version(v1_path, 1);

    // v1 has the original seven guest sections and no native MODS identity;
    // an empty catalog may restore it exactly.
    bus.write8(0x02000000u, 0xaa);
    error.clear();
    check_bool("snapshot_legacy_v1", "empty catalog loads",
               debug::load_state(v1_path.string().c_str(), context, &error), true);
    check_eq("snapshot_legacy_v1", "guest restored", bus.read8(0x02000000u), 0x11u);

    SnapshotModValue provider_value{0x7777u};
    debug::ModStateRegistry catalog;
    const debug::ModStateProvider provider = {
        "test.legacy", 1, &provider_value,
        snapshot_mod_save, snapshot_mod_preflight, snapshot_mod_restore};
    check_bool("snapshot_legacy_v1", "register provider",
               catalog.register_provider(provider, &error), true);
    debug::SnapshotContext provider_context = context;
    provider_context.mod_state = &catalog;
    bus.write8(0x02000000u, 0xbb);
    error.clear();
    check_bool("snapshot_legacy_v1", "provider catalog rejected",
               debug::load_state(v1_path.string().c_str(), provider_context, &error), false);
    check_bool("snapshot_legacy_v1", "rejection names migration",
               error.find("no MODS migration") != std::string::npos, true);
    check_eq("snapshot_legacy_v1", "provider rejection leaves guest", bus.read8(0x02000000u), 0xbbu);
    check_eq("snapshot_legacy_v1", "provider not restored", provider_value.restores, 0);

    // The legacy branch frames every declared section and consumes the entire
    // container before guest restoration; an unframed tail is never ignored.
    std::filesystem::copy_file(v1_path, malformed_path, std::filesystem::copy_options::overwrite_existing, ec);
    { std::ofstream malformed(malformed_path, std::ios::binary | std::ios::app); malformed.put('\0'); }
    bus.write8(0x02000000u, 0xcc);
    error.clear();
    check_bool("snapshot_legacy_v1", "trailing data rejected",
               debug::load_state(malformed_path.string().c_str(), context, &error), false);
    check_eq("snapshot_legacy_v1", "malformed leaves guest", bus.read8(0x02000000u), 0xccu);

    std::filesystem::remove(v2_path, ec);
    std::filesystem::remove(v1_path, ec);
    std::filesystem::remove(malformed_path, ec);
}

void test_v2_trailing_container_rejects_before_guest_mutation() {
    namespace debug = gbarecomp::debug;
    gba::GbaBus bus;
    gba::GbaPpu ppu;
    debug::ModStateRegistry empty_catalog;
    debug::SnapshotContext context;
    context.bus = &bus;
    context.ppu = &ppu;
    context.rom_sha1 = "0123456789012345678901234567890123456789";
    context.mod_state = &empty_catalog;
    std::string error;
    std::error_code ec;
    const auto path = std::filesystem::temp_directory_path() /
        "gbarecomp-v2-trailing-container.gbas";
    std::filesystem::remove(path, ec);

    bus.write8(0x02000000u, 0x11);
    check_bool("snapshot_v2_trailing", "write source",
               debug::save_state(path.string().c_str(), context, &error), true);
    { std::ofstream malformed(path, std::ios::binary | std::ios::app); malformed.put('\0'); }

    // The structural framing gate must run before CPU/BUS/PPU deserialization.
    bus.write8(0x02000000u, 0xA5);
    error.clear();
    check_bool("snapshot_v2_trailing", "trailing data rejected",
               debug::load_state(path.string().c_str(), context, &error), false);
    check_bool("snapshot_v2_trailing", "error identifies trailing data",
               error.find("trailing container data") != std::string::npos, true);
    check_eq("snapshot_v2_trailing", "guest remains untouched",
             bus.read8(0x02000000u), 0xA5u);
    std::filesystem::remove(path, ec);
}

void test_sram_bus_width_and_region_mirroring() {
    gba::GbaBus bus;
    bus.save().configure_sram(32 * 1024);

    bus.write16(0x0E000001u, 0xABCDu);
    check_eq("sram_bus", "write16_low_byte", bus.read8(0x0E000001u), 0xCDu);
    check_eq("sram_bus", "read16_replicates", bus.read16(0x0E000001u), 0xCDCDu);

    // The 32 KiB SRAM address lines mirror throughout the 0E/0F save region.
    bus.write32(0x0F008002u, 0x12345678u);
    check_eq("sram_bus", "region_mirror", bus.read8(0x0E000002u), 0x78u);
    check_eq("sram_bus", "read32_replicates", bus.read32(0x0E000002u),
             0x78787878u);
}

void test_bios_undocumented_io_write() {
    gba::GbaBus bus;
    check_eq("undoc_io_410", "initial_unhandled",
             bus.io().unmapped_count(), static_cast<std::size_t>(0));
    bus.write8(0x04000410u, 0xFFu);
    check_eq("undoc_io_410", "bios_write_handled",
             bus.io().unmapped_count(), static_cast<std::size_t>(0));
    // Neighboring extended-IO writes remain loud; only the observed register
    // is admitted.
    bus.write8(0x04000411u, 0xFFu);
    check_eq("undoc_io_410", "neighbor_unhandled",
             bus.io().unmapped_count(), static_cast<std::size_t>(1));
}

void test_bios_window_writes_are_ignored() {
    gba::GbaBus bus;
    check_eq("bios_write", "initial_unmapped", bus.unmapped_count(),
             static_cast<std::size_t>(0));
    bus.write8(0x00000001u, 0x12u);
    bus.write16(0x00000002u, 0x3456u);
    bus.write32(0x00000004u, 0x789ABCDEu);
    check_eq("bios_write", "ignored_without_unmapped_diagnostic",
             bus.unmapped_count(), static_cast<std::size_t>(0));
}

void test_matrix_memory_mapper() {
    constexpr std::size_t kRomSize = 64u * 1024u * 1024u;
    std::vector<uint8_t> rom(kRomSize, 0);
    rom[0xAC] = static_cast<uint8_t>('M');
    for (std::size_t i = 0; i < rom.size(); ++i) {
        rom[i] = static_cast<uint8_t>((i >> 9u) & 0xFFu);
    }
    rom[0xAC] = static_cast<uint8_t>('M');

    gba::GbaBus bus;
    bus.set_rom(rom.data(), rom.size());
    check_bool("matrix", "detected", bus.matrix().active(), true);

    // Reset maps virtual page 8 (0x1000) to physical page 1 (0x0200).
    check_eq("matrix", "reset_lower", bus.read8(0x08000000u), 0u);
    check_eq("matrix", "reset_overlap", bus.read8(0x08001000u), 1u);

    // Map four 512-byte pages from the physical second 32 MiB into virtual
    // pages 4..7, then commit with command 1.
    bus.write32(0x08800104u, 0x02000000u);
    bus.write32(0x08800108u, 0x00000800u);
    bus.write32(0x0880010Cu, 4u);
    bus.write32(0x08800100u, 1u);
    check_eq("matrix", "map_command_count", bus.matrix().map_command_count(),
             1u);
    check_eq("matrix", "highest_physical_end",
             bus.matrix().highest_physical_end(), 0x02000800u);
    check_eq("matrix", "mapped_first", bus.read8(0x08000800u), 0u);
    rom[0x02000000u] = 0xA5u;
    rom[0x02000600u] = 0x5Au;
    check_eq("matrix", "physical_second_half", bus.read8(0x08000800u), 0xA5u);
    check_eq("matrix", "mapped_last", bus.read8(0x08000E00u), 0x5Au);
    check_eq("matrix", "command_write_is_handled", bus.unmapped_count(),
             static_cast<std::size_t>(0));

    // Any ROM wait-state mirror selects the same command block.
    bus.write32(0x0A800104u, 0x02001000u);
    bus.write32(0x0A800108u, 0x00000000u);
    bus.write32(0x0A80010Cu, 1u);
    bus.write32(0x0A800100u, 0x11u);
    rom[0x02001000u] = 0xC3u;
    check_eq("matrix", "waitstate_mirror", bus.read8(0x0C000000u), 0xC3u);
    check_eq("matrix", "mirror_map_command_count",
             bus.matrix().map_command_count(), 2u);
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

void test_eeprom_persistence_bytes_and_dirty() {
    gba::GbaSave save;
    std::array<uint8_t, 16> persisted{};
    for (std::size_t i = 0; i < persisted.size(); ++i) {
        persisted[i] = static_cast<uint8_t>(0xA0u + i);
    }

    check_bool("eeprom_persist", "load_before_config",
               save.load_eeprom_bytes(persisted.data(), persisted.size()),
               false);
    save.configure_eeprom(8 * 1024);
    check_bool("eeprom_persist", "blank_dirty", save.dirty(), false);
    check_bool("eeprom_persist", "load",
               save.load_eeprom_bytes(persisted.data(), persisted.size()),
               true);
    check_bool("eeprom_persist", "load_dirty", save.dirty(), false);

    auto bytes = save.eeprom_bytes();
    check_eq("eeprom_persist", "size", bytes.size(),
             static_cast<std::size_t>(8 * 1024));
    for (std::size_t i = 0; i < persisted.size(); ++i) {
        check_eq("eeprom_persist", "loaded_prefix", bytes[i], persisted[i]);
    }
    check_eq("eeprom_persist", "loaded_fill", bytes[32], 0xFFu);

    const uint8_t pattern[8] = {0x12, 0x34, 0x56, 0x78,
                                0x9a, 0xbc, 0xde, 0xf0};
    eeprom_send_write(save, 0, pattern);
    check_bool("eeprom_persist", "write_dirty", save.dirty(), true);
    save.clear_dirty();
    eeprom_send_write(save, 0, pattern);
    check_bool("eeprom_persist", "same_write_clean", save.dirty(), false);
}

}  // namespace

// WAITCNT / internal memory control drive every bus cost: data accesses
// (incl. DMA), opcode fetches and the GamePak prefetch state, which all
// save-state paths carry (GBATEK "GBA System Control"; mGBA memory.c).
void test_waitstates() {
    const char* t = "waitstates";
    gba::GbaBus bus;
    check_eq(t, "rom_n32", bus.access_cycles(0x08000000u, 4, false), 8u);
    check_eq(t, "rom_s32", bus.access_cycles(0x08000000u, 4, true), 6u);
    check_eq(t, "rom_n16", bus.access_cycles(0x08000000u, 2, false), 5u);
    check_eq(t, "rom_s16", bus.access_cycles(0x08000000u, 2, true), 3u);
    check_eq(t, "ws2_s16", bus.access_cycles(0x0C000000u, 2, true), 9u);
    check_eq(t, "sram", bus.access_cycles(0x0E000000u, 4, true), 5u);
    check_eq(t, "ewram32", bus.access_cycles(0x02000000u, 4, false), 6u);
    check_eq(t, "iwram32", bus.access_cycles(0x03000000u, 4, false), 1u);
    check_eq(t, "vram32", bus.access_cycles(0x06000000u, 4, false), 2u);
    check_eq(t, "fetch_thumb_rom_s", bus.code_wait(0x08000000u, true, true), 2u);
    check_eq(t, "fetch_arm_rom_n", bus.code_wait(0x08000000u, false, false), 7u);
    check_eq(t, "fetch_iwram", bus.code_wait(0x03000000u, false, false), 0u);
    check_eq(t, "no_prefetch", bus.prefetch_stall(2, 0x08000100u, true), 2);

    // Bit 15 (and 13) read as zero; ROM 3/1, SRAM 8, prefetch on.
    bus.write16(0x04000204u, 0xC317u);
    check_eq(t, "waitcnt_masked", bus.read16(0x04000204u), 0x4317u);
    check_eq(t, "ws0_n16", bus.access_cycles(0x08000000u, 2, false), 4u);
    check_eq(t, "ws0_s16", bus.access_cycles(0x09000000u, 2, true), 2u);
    check_eq(t, "ws0_n32", bus.access_cycles(0x08000000u, 4, false), 6u);
    check_eq(t, "ws1_n16", bus.access_cycles(0x0A000000u, 2, false), 5u);
    check_eq(t, "ws2_n16", bus.access_cycles(0x0D000000u, 2, false), 9u);
    check_eq(t, "sram8", bus.access_cycles(0x0E000000u, 1, false), 9u);
    check_eq(t, "fetch_thumb_3_1", bus.code_wait(0x08000000u, true, true), 1u);
    // mGBA GBAMemoryStall: a 2-cycle IWRAM load by THUMB ROM code at 3/1
    // becomes -2 (the N fetch turns S, one prefetched halfword is free).
    check_eq(t, "prefetch_stall", bus.prefetch_stall(2, 0x08000100u, true), -2);
    check_eq(t, "prefetched_pc", bus.wait_table()->last_prefetched_pc, 0x08000104u);
    // A byte write to the high half toggles the prefetch buffer alone.
    bus.write8(0x04000205u, 0x03u);
    check_eq(t, "prefetch_off", bus.prefetch_stall(2, 0x08000100u, true), 2);
    bus.write8(0x04000205u, 0x43u);

    // Internal memory control: reset value, mirrors every 64 KiB, EWRAM wait
    // 15 - bits 24-27; 0 wait states (0Fh) is rejected like mGBA does.
    check_eq(t, "memctl_reset", bus.read32(0x04000800u), 0x0D000020u);
    bus.write32(0x04010800u, 0x0E000020u);
    check_eq(t, "memctl_mirror", bus.read32(0x04000800u), 0x0E000020u);
    check_eq(t, "ewram1_32", bus.access_cycles(0x02000000u, 4, false), 4u);
    check_eq(t, "ewram1_16", bus.access_cycles(0x02000000u, 2, true), 2u);
    check_eq(t, "fetch_ewram1", bus.code_wait(0x02000000u, true, true), 1u);
    bus.write8(0x04000803u, 0x0Fu);
    check_eq(t, "memctl_written", bus.read32(0x04000800u), 0x0F000020u);
    check_eq(t, "ewram_zero_rejected", bus.access_cycles(0x02000000u, 2, true), 2u);

    // Save state (bus + IO sections, then the orchestrator's refresh).
    gbarecomp::debug::SnapshotWriter bw, iw;
    bus.serialize(bw);
    bus.io().serialize(iw);
    gba::GbaBus restored;
    gbarecomp::debug::SnapshotReader br(bw.buffer().data(), bw.buffer().size());
    gbarecomp::debug::SnapshotReader ir(iw.buffer().data(), iw.buffer().size());
    restored.deserialize(br);
    restored.io().deserialize(ir);
    restored.refresh_waitstates();
    check_eq(t, "snap_memctl", restored.read32(0x04000800u), 0x0F000020u);
    check_eq(t, "snap_ewram", restored.access_cycles(0x02000000u, 4, true), 4u);
    check_eq(t, "snap_ws0", restored.access_cycles(0x08000000u, 2, false), 4u);
    check_eq(t, "snap_prefetched_pc", restored.wait_table()->last_prefetched_pc,
             0x08000104u);
    check_eq(t, "snap_prefetch", restored.wait_table()->prefetch, 1u);

    // Multiplayer/netplay device state.
    gba::GbaPpu ppu, ppu2;
    bus.rtc().set_emulated_clock(0);  // sessions always run the emulated RTC
    const auto blob = gba::save_device_state(bus, ppu);
    gba::GbaBus staged;
    staged.rtc().set_emulated_clock(0);
    check_bool(t, "sim_load", gba::load_device_state(staged, ppu2, blob), true);
    check_eq(t, "sim_memctl", staged.read32(0x04000800u), 0x0F000020u);
    check_eq(t, "sim_ewram", staged.access_cycles(0x02000000u, 4, true), 4u);
    check_eq(t, "sim_ws0", staged.access_cycles(0x08000000u, 2, false), 4u);
    check_eq(t, "sim_prefetched_pc", staged.wait_table()->last_prefetched_pc,
             0x08000104u);
}

int main() {
    test_minishcap_like();
    test_corrupt_header();
    test_no_save_signature();
    test_sram_signature();
    test_sram_controller_and_snapshot();
    test_snapshot_mods_preflight_before_guest_restore();
    test_legacy_v1_snapshot_empty_catalog_only();
    test_v2_trailing_container_rejects_before_guest_mutation();
    test_sram_bus_width_and_region_mirroring();
    test_bios_undocumented_io_write();
    test_bios_window_writes_are_ignored();
    test_matrix_memory_mapper();
    test_flash1m_beats_flash();
    test_eeprom_8k_read_write();
    test_eeprom_persistence_bytes_and_dirty();
    test_waitstates();
    if (failures) {
        std::printf("\n%d failure(s)\n", failures);
        return 1;
    }
    std::printf("bus_tests (rom header): OK\n");
    return 0;
}
