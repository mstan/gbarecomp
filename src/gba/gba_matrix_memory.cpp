// gba_matrix_memory.cpp — Matrix Memory mapper for 64 MiB GBA Video carts.

#include "gba_matrix_memory.h"

#include <cstdio>

#include "snapshot.h"

namespace gba {

namespace {

constexpr uint32_t kPhysicalMask = 0x03FFFFFFu;
constexpr uint32_t kVirtualMask = 0x007FFFFFu;

}  // namespace

void GbaMatrixMemory::configure(const uint8_t* rom, std::size_t rom_size) {
    rom_size_ = rom_size;
    active_ = rom && rom_size > kNormalRomLimit &&
              rom_size > 0xACu && rom[0xAC] == static_cast<uint8_t>('M');
    reset();
}

void GbaMatrixMemory::reset() {
    mappings_.fill(0);
    command_ = 0;
    physical_address_ = 0;
    virtual_address_ = 0;
    transfer_size_ = 0x1000u;
    map_command_count_ = 0;
    highest_physical_end_ = 0;
    if (!active_) return;

    // Reset exposes the cartridge header/boot block at 0x0000 and a second,
    // overlapping 4 KiB window at 0x1000. This is the Matrix chip's observed
    // power-on layout and is required before the first explicit map command.
    physical_address_ = 0;
    virtual_address_ = 0;
    remap();
    physical_address_ = 0x200u;
    virtual_address_ = 0x1000u;
    remap();
}

bool GbaMatrixMemory::translate(std::size_t virtual_offset,
                                std::size_t width,
                                std::size_t* physical_offset) const {
    if (!physical_offset || width == 0) return false;

    std::size_t physical = virtual_offset;
    if (active_ && virtual_offset < kApertureSize) {
        const std::size_t page = virtual_offset / kPageSize;
        physical = static_cast<std::size_t>(mappings_[page]) +
                   (virtual_offset & (kPageSize - 1u));
    }
    if (physical > rom_size_ || width > rom_size_ - physical) return false;
    *physical_offset = physical;
    return true;
}

bool GbaMatrixMemory::is_register_address(uint32_t addr) {
    return (addr & 0x01FFFF00u) == 0x00800100u;
}

void GbaMatrixMemory::write16(uint32_t addr, uint16_t value) {
    if (!active_ || !is_register_address(addr)) return;
    const uint32_t reg = addr & 0x3Cu;
    uint32_t previous = 0;
    switch (reg) {
        case 0x0: previous = command_; break;
        case 0x4: previous = physical_address_; break;
        case 0x8: previous = virtual_address_; break;
        case 0xC: previous = transfer_size_; break;
        default: return;
    }
    write_register(reg, static_cast<uint32_t>(value) |
                            (previous & 0xFFFF0000u));
}

void GbaMatrixMemory::write32(uint32_t addr, uint32_t value) {
    if (!active_ || !is_register_address(addr)) return;
    write_register(addr & 0x3Cu, value);
}

void GbaMatrixMemory::write_register(uint32_t reg, uint32_t value) {
    switch (reg) {
        case 0x0:
            command_ = value;
            if ((value == 0x01u || value == 0x11u) && remap()) {
                ++map_command_count_;
            }
            return;
        case 0x4:
            physical_address_ = value & kPhysicalMask;
            return;
        case 0x8:
            virtual_address_ = value & kVirtualMask;
            return;
        case 0xC:
            if (value != 0) transfer_size_ = value << 9u;
            return;
        default:
            return;
    }
}

bool GbaMatrixMemory::remap() {
    // The chip exposes sixteen 512-byte aperture pages. Both destination and
    // size must be 512-byte aligned and wholly contained in that aperture.
    if ((virtual_address_ & 0xFFFFE1FFu) != 0 ||
        (transfer_size_ & 0xFFFFE1FFu) != 0 ||
        transfer_size_ == 0) {
        return false;
    }
    const uint64_t end = static_cast<uint64_t>(virtual_address_) +
                         static_cast<uint64_t>(transfer_size_) - 1u;
    if ((end & 0xFFFFE000ull) != 0) return false;

    const std::size_t start = virtual_address_ >> 9u;
    const std::size_t count = transfer_size_ >> 9u;
    for (std::size_t i = 0; i < count; ++i) {
        mappings_[(start + i) & (kPageCount - 1u)] =
            physical_address_ + static_cast<uint32_t>(i * kPageSize);
    }
    const uint64_t physical_end =
        static_cast<uint64_t>(physical_address_) + transfer_size_;
    if (physical_end > highest_physical_end_) {
        highest_physical_end_ = static_cast<uint32_t>(
            physical_end > 0xFFFFFFFFull ? 0xFFFFFFFFull : physical_end);
    }
    return true;
}

void GbaMatrixMemory::serialize(gbarecomp::debug::SnapshotWriter& w) const {
    w.boolean(active_);
    w.u64(static_cast<uint64_t>(rom_size_));
    w.u32(command_);
    w.u32(physical_address_);
    w.u32(virtual_address_);
    w.u32(transfer_size_);
    w.u32(map_command_count_);
    w.u32(highest_physical_end_);
    for (uint32_t mapping : mappings_) w.u32(mapping);
}

void GbaMatrixMemory::deserialize(gbarecomp::debug::SnapshotReader& r) {
    const bool snapshot_active = r.boolean();
    const std::size_t snapshot_rom_size =
        static_cast<std::size_t>(r.u64());
    command_ = r.u32();
    physical_address_ = r.u32();
    virtual_address_ = r.u32();
    transfer_size_ = r.u32();
    map_command_count_ = r.u32();
    highest_physical_end_ = r.u32();
    for (uint32_t& mapping : mappings_) mapping = r.u32();

    // ROM identity is validated by the snapshot orchestrator. Still refuse to
    // activate a mapper state against a differently-sized live image.
    active_ = snapshot_active && snapshot_rom_size == rom_size_ &&
              rom_size_ > kNormalRomLimit;
    if (!active_) reset();
}

}  // namespace gba
