// gba_save.h — STUB. Cartridge save chip controller.
//
// Detect on ROM scan: the standard heuristic is to grep the ROM for
// SRAM_Vnnn / FLASH_Vnnn / FLASH512_Vnnn / FLASH1M_Vnnn / EEPROM_Vnnn
// strings (Nintendo SDK signatures). Per-chip state machines:
//   - SRAM 32K: plain byte read/write at 0x0E000000.
//   - Flash 64K (512Kbit) / 128K (1Mbit): command-driven; manufacturer
//     IDs differ. Two banks for 128K (selected by command).
//   - EEPROM 512 byte / 8 KB: serial protocol over DMA from a tiny ROM
//     window at the top of the cartridge address space.
//
// Reference: GBATEK § "GBA Backup Memory".

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gbarecomp::debug { class SnapshotWriter; class SnapshotReader; }

namespace gba {

class GbaSave {
public:
    GbaSave();
    ~GbaSave();

    void configure_eeprom(std::size_t bytes);
    bool eeprom_enabled() const { return eeprom_enabled_; }

    void eeprom_write_bit(uint16_t value);
    uint16_t eeprom_read_bit();

    std::size_t eeprom_size() const { return eeprom_size_; }
    bool load_eeprom_bytes(const uint8_t* data, std::size_t bytes);
    std::vector<uint8_t> eeprom_bytes() const;
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    // Save-state serialization (chip config + EEPROM bytes + in-flight
    // serial command/read state). See debug/snapshot.h.
    void serialize(gbarecomp::debug::SnapshotWriter& w) const;
    void deserialize(gbarecomp::debug::SnapshotReader& r);

private:
    static constexpr std::size_t kMaxEepromSize = 8 * 1024;

    bool eeprom_enabled_ = false;
    bool dirty_ = false;
    std::size_t eeprom_size_ = 0;
    uint32_t eeprom_addr_bits_ = 0;
    uint32_t eeprom_block_mask_ = 0;
    std::array<uint8_t, kMaxEepromSize> eeprom_{};

    std::vector<uint8_t> command_bits_;
    bool read_active_ = false;
    uint32_t read_byte_offset_ = 0;
    uint32_t read_bit_index_ = 0;

    uint32_t command_address() const;
    void finish_eeprom_read();
    void finish_eeprom_write();
};

}  // namespace gba
