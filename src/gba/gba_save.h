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

    // Cartridge FLASH controller (Macronix MX29L010 1 Mbit / 128 KB, the
    // chip every pret Gen3 game uses; also models 512 Kbit / 64 KB). The
    // chip is command-driven through writes to the save region (0x0E...):
    // an unlock sequence (0xAA @ 0x5555, 0x55 @ 0x2AAA) precedes each
    // command (0x90 enter-ID, 0xA0 program-byte, 0x80.. erase, 0xB0
    // bank-select, 0xF0 reset). 1 Mbit parts page two 64 KB banks through
    // the 64 KB window. See agb_flash.c in the pret decomp + GBATEK
    // § "GBA Backup Flash ROM".
    //   bytes: total chip size (0x20000 for 1 Mbit, 0x10000 for 512 Kbit).
    void configure_flash(std::size_t bytes);
    bool flash_enabled() const { return flash_enabled_; }
    std::size_t flash_size() const { return flash_size_; }
    // off = address within the save region (addr - 0x0E000000); masked to
    // the 64 KB window internally and combined with the selected bank.
    uint8_t flash_read(uint32_t off);
    void    flash_write(uint32_t off, uint8_t value);
    bool load_flash_bytes(const uint8_t* data, std::size_t bytes);
    std::vector<uint8_t> flash_bytes() const;

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

    // ---- FLASH controller state ----
    // Command-sequence FSM. Each command is preceded by the two-cycle
    // unlock (0xAA @ 0x5555, 0x55 @ 0x2AAA); erase needs a second unlock.
    enum class FlashState : uint8_t {
        Idle, Unlock1, Unlock2,
        EraseUnlock0, EraseUnlock1, EraseUnlock2,
        Program, Bank,
    };
    static constexpr uint32_t kFlashBankBytes = 0x10000;  // 64 KB window

    bool flash_enabled_ = false;
    std::size_t flash_size_ = 0;       // total chip bytes (0x20000 / 0x10000)
    uint32_t flash_banks_ = 1;         // 2 for 1 Mbit, 1 for 512 Kbit
    uint8_t flash_maker_ = 0xC2;       // Macronix
    uint8_t flash_device_ = 0x09;      // MX29L010 (1 Mbit)
    FlashState flash_state_ = FlashState::Idle;
    bool flash_id_mode_ = false;
    uint8_t flash_bank_ = 0;
    std::vector<uint8_t> flash_data_;
};

}  // namespace gba
