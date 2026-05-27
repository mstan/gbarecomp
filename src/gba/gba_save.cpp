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

}  // namespace gba
