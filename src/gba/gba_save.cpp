#include "gba_save.h"

#include <algorithm>

#include "snapshot.h"

namespace gba {

GbaSave::GbaSave() {
    eeprom_.fill(0xFF);
}

GbaSave::~GbaSave() = default;

void GbaSave::serialize(gbarecomp::debug::SnapshotWriter& w) const {
    w.boolean(eeprom_enabled_);
    w.boolean(dirty_);
    w.u64(eeprom_size_);
    w.u32(eeprom_addr_bits_);
    w.u32(eeprom_block_mask_);
    w.bytes(eeprom_.data(), eeprom_.size());
    w.u32(static_cast<uint32_t>(command_bits_.size()));
    if (!command_bits_.empty()) {
        w.bytes(command_bits_.data(), command_bits_.size());
    }
    w.boolean(read_active_);
    w.u32(read_byte_offset_);
    w.u32(read_bit_index_);

    // FLASH state — appended after the EEPROM block so an older savestate
    // (whose SAVE section ends here) still deserializes: the reader is
    // bounded to the section length, and deserialize() only reads these
    // fields when bytes remain. No snapshot-version bump required.
    w.boolean(flash_enabled_);
    w.u64(flash_size_);
    w.u32(flash_banks_);
    w.u8(flash_maker_);
    w.u8(flash_device_);
    w.u8(static_cast<uint8_t>(flash_state_));
    w.boolean(flash_id_mode_);
    w.u8(flash_bank_);
    w.u32(static_cast<uint32_t>(flash_data_.size()));
    if (!flash_data_.empty()) {
        w.bytes(flash_data_.data(), flash_data_.size());
    }
}

void GbaSave::deserialize(gbarecomp::debug::SnapshotReader& r) {
    eeprom_enabled_    = r.boolean();
    dirty_             = r.boolean();
    eeprom_size_       = static_cast<std::size_t>(r.u64());
    eeprom_addr_bits_  = r.u32();
    eeprom_block_mask_ = r.u32();
    r.bytes(eeprom_.data(), eeprom_.size());
    uint32_t cmd_len = r.u32();
    command_bits_.assign(cmd_len, 0);
    if (cmd_len) r.bytes(command_bits_.data(), cmd_len);
    read_active_       = r.boolean();
    read_byte_offset_  = r.u32();
    read_bit_index_    = r.u32();

    // FLASH state (optional trailing block — absent in pre-flash
    // savestates, whose SAVE section ends after the EEPROM fields).
    if (r.remaining() == 0) return;
    flash_enabled_ = r.boolean();
    flash_size_    = static_cast<std::size_t>(r.u64());
    flash_banks_   = r.u32();
    flash_maker_   = r.u8();
    flash_device_  = r.u8();
    flash_state_   = static_cast<FlashState>(r.u8());
    flash_id_mode_ = r.boolean();
    flash_bank_    = r.u8();
    uint32_t flash_len = r.u32();
    flash_data_.assign(flash_len, 0xFF);
    if (flash_len) r.bytes(flash_data_.data(), flash_len);
}

void GbaSave::configure_eeprom(std::size_t bytes) {
    if (bytes == 0 || bytes > kMaxEepromSize) bytes = kMaxEepromSize;
    eeprom_enabled_ = true;
    dirty_ = false;
    eeprom_size_ = bytes;
    eeprom_addr_bits_ = (bytes <= 512) ? 6u : 14u;
    eeprom_block_mask_ = static_cast<uint32_t>((bytes / 8u) - 1u);
    eeprom_.fill(0xFF);
    command_bits_.clear();
    read_active_ = false;
    read_byte_offset_ = 0;
    read_bit_index_ = 0;
}

bool GbaSave::load_eeprom_bytes(const uint8_t* data, std::size_t bytes) {
    if (!eeprom_enabled_ || (!data && bytes != 0) || bytes > eeprom_size_) {
        return false;
    }
    eeprom_.fill(0xFF);
    if (bytes != 0) std::copy(data, data + bytes, eeprom_.begin());
    command_bits_.clear();
    read_active_ = false;
    read_byte_offset_ = 0;
    read_bit_index_ = 0;
    dirty_ = false;
    return true;
}

std::vector<uint8_t> GbaSave::eeprom_bytes() const {
    if (!eeprom_enabled_) return {};
    return std::vector<uint8_t>(eeprom_.begin(), eeprom_.begin() + eeprom_size_);
}

uint32_t GbaSave::command_address() const {
    uint32_t addr = 0;
    for (uint32_t i = 0; i < eeprom_addr_bits_; ++i) {
        addr = (addr << 1) | command_bits_[2u + i];
    }
    return addr & eeprom_block_mask_;
}

void GbaSave::finish_eeprom_read() {
    uint32_t block = command_address();
    read_byte_offset_ = block * 8u;
    read_bit_index_ = 0;
    read_active_ = true;
    command_bits_.clear();
}

void GbaSave::finish_eeprom_write() {
    uint32_t block = command_address();
    uint32_t byte_offset = block * 8u;
    uint32_t data_start = 2u + eeprom_addr_bits_;
    for (uint32_t byte = 0; byte < 8; ++byte) {
        uint8_t v = 0;
        for (uint32_t bit = 0; bit < 8; ++bit) {
            v = static_cast<uint8_t>(
                (v << 1) | command_bits_[data_start + byte * 8u + bit]);
        }
        if (byte_offset + byte < eeprom_size_) {
            if (eeprom_[byte_offset + byte] != v) dirty_ = true;
            eeprom_[byte_offset + byte] = v;
        }
    }
    command_bits_.clear();
    read_active_ = false;
    read_bit_index_ = 0;
}

void GbaSave::eeprom_write_bit(uint16_t value) {
    if (!eeprom_enabled_) return;

    command_bits_.push_back(static_cast<uint8_t>(value & 1u));
    if (command_bits_.size() < 2) return;

    uint8_t op = static_cast<uint8_t>((command_bits_[0] << 1) |
                                      command_bits_[1]);
    const std::size_t read_bits = 2u + eeprom_addr_bits_ + 1u;
    const std::size_t write_bits = 2u + eeprom_addr_bits_ + 64u + 1u;

    if (op == 0b11 && command_bits_.size() == read_bits) {
        finish_eeprom_read();
        return;
    }
    if (op == 0b10 && command_bits_.size() == write_bits) {
        finish_eeprom_write();
        return;
    }

    if (op != 0b10 && op != 0b11) {
        command_bits_.clear();
        read_active_ = false;
        read_bit_index_ = 0;
        return;
    }
    if (command_bits_.size() > write_bits) {
        command_bits_.clear();
        read_active_ = false;
        read_bit_index_ = 0;
    }
}

uint16_t GbaSave::eeprom_read_bit() {
    if (!eeprom_enabled_) return 1;
    if (!read_active_) return 1;

    uint16_t bit = 0;
    if (read_bit_index_ >= 4 && read_bit_index_ < 68) {
        uint32_t data_bit = read_bit_index_ - 4u;
        uint32_t byte = data_bit / 8u;
        uint32_t shift = 7u - (data_bit % 8u);
        if (read_byte_offset_ + byte < eeprom_size_) {
            bit = static_cast<uint16_t>(
                (eeprom_[read_byte_offset_ + byte] >> shift) & 1u);
        }
    }

    ++read_bit_index_;
    if (read_bit_index_ >= 68) {
        read_active_ = false;
        read_bit_index_ = 0;
    }
    return bit;
}

// ---------------------------------------------------------------------------
// FLASH controller (Macronix MX29L010 command set). Mirrors the protocol the
// pret decomp's agb_flash drivers drive: an unlock sequence precedes each
// command, ID mode exposes the maker/device bytes, 1 Mbit parts page two
// 64 KB banks through the 64 KB window.
// ---------------------------------------------------------------------------

void GbaSave::configure_flash(std::size_t bytes) {
    if (bytes != 0x10000 && bytes != 0x20000) bytes = 0x20000;  // default 1 Mbit
    flash_enabled_ = true;
    dirty_ = false;
    flash_size_ = bytes;
    flash_banks_ = static_cast<uint32_t>(bytes / kFlashBankBytes);
    if (flash_banks_ == 0) flash_banks_ = 1;
    flash_maker_ = 0xC2;   // Macronix
    flash_device_ = 0x09;  // MX29L010 (1 Mbit); accepted by FLASH1M & 512K drivers
    flash_state_ = FlashState::Idle;
    flash_id_mode_ = false;
    flash_bank_ = 0;
    flash_data_.assign(bytes, 0xFF);
}

bool GbaSave::load_flash_bytes(const uint8_t* data, std::size_t bytes) {
    if (!flash_enabled_ || (!data && bytes != 0) || bytes > flash_size_) {
        return false;
    }
    flash_data_.assign(flash_size_, 0xFF);
    if (bytes != 0) std::copy(data, data + bytes, flash_data_.begin());
    flash_state_ = FlashState::Idle;
    flash_id_mode_ = false;
    flash_bank_ = 0;
    dirty_ = false;
    return true;
}

std::vector<uint8_t> GbaSave::flash_bytes() const {
    if (!flash_enabled_) return {};
    return flash_data_;
}

uint8_t GbaSave::flash_read(uint32_t off) {
    if (!flash_enabled_) return 0xFF;
    off &= 0xFFFFu;
    if (flash_id_mode_) {
        // Autoselect: A0=0 -> manufacturer, A0=1 -> device.
        if (off == 0x0000) return flash_maker_;
        if (off == 0x0001) return flash_device_;
        return 0xFF;
    }
    std::size_t idx = static_cast<std::size_t>(flash_bank_) * kFlashBankBytes + off;
    if (idx < flash_data_.size()) return flash_data_[idx];
    return 0xFF;
}

void GbaSave::flash_write(uint32_t off, uint8_t value) {
    if (!flash_enabled_) return;
    off &= 0xFFFFu;

    // 0xF0 resets the chip / leaves ID mode from any state (single cycle).
    if (value == 0xF0 && flash_state_ != FlashState::Program) {
        flash_id_mode_ = false;
        flash_state_ = FlashState::Idle;
        return;
    }

    switch (flash_state_) {
    case FlashState::Idle:
        if (off == 0x5555 && value == 0xAA) flash_state_ = FlashState::Unlock1;
        break;
    case FlashState::Unlock1:
        flash_state_ = (off == 0x2AAA && value == 0x55)
                           ? FlashState::Unlock2 : FlashState::Idle;
        break;
    case FlashState::Unlock2:
        if (off == 0x5555) {
            switch (value) {
            case 0x90: flash_id_mode_ = true;  flash_state_ = FlashState::Idle; break;
            case 0xA0: flash_state_ = FlashState::Program; break;
            case 0xB0: flash_state_ = FlashState::Bank;    break;
            case 0x80: flash_state_ = FlashState::EraseUnlock0; break;
            default:   flash_state_ = FlashState::Idle; break;
            }
        } else {
            flash_state_ = FlashState::Idle;
        }
        break;
    case FlashState::EraseUnlock0:
        flash_state_ = (off == 0x5555 && value == 0xAA)
                           ? FlashState::EraseUnlock1 : FlashState::Idle;
        break;
    case FlashState::EraseUnlock1:
        flash_state_ = (off == 0x2AAA && value == 0x55)
                           ? FlashState::EraseUnlock2 : FlashState::Idle;
        break;
    case FlashState::EraseUnlock2: {
        if (off == 0x5555 && value == 0x10) {
            std::fill(flash_data_.begin(), flash_data_.end(), 0xFF);  // chip erase
            dirty_ = true;
        } else if (value == 0x30) {
            // Sector erase: off is the 4 KB sector base within the bank.
            std::size_t base = static_cast<std::size_t>(flash_bank_) *
                                   kFlashBankBytes + (off & 0xF000u);
            for (uint32_t i = 0; i < 0x1000u; ++i) {
                if (base + i < flash_data_.size()) flash_data_[base + i] = 0xFF;
            }
            dirty_ = true;
        }
        flash_state_ = FlashState::Idle;
        break;
    }
    case FlashState::Program: {
        std::size_t idx = static_cast<std::size_t>(flash_bank_) *
                              kFlashBankBytes + off;
        if (idx < flash_data_.size() && flash_data_[idx] != value) {
            flash_data_[idx] = value;
            dirty_ = true;
        }
        flash_state_ = FlashState::Idle;
        break;
    }
    case FlashState::Bank:
        // Bank select: bank number written to 0x0000.
        flash_bank_ = (flash_banks_ > 1)
                          ? static_cast<uint8_t>(value & (flash_banks_ - 1)) : 0;
        flash_state_ = FlashState::Idle;
        break;
    }
}

}  // namespace gba
