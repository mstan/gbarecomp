// gba_matrix_memory.h — Matrix Memory mapper used by 64 MiB GBA Video carts.
//
// A normal GBA exposes a 32 MiB cartridge ROM window. Matrix Semiconductor
// movie cartridges keep the executable in that ordinary window and page
// 512-byte chunks from a larger physical ROM into a small 8 KiB aperture at
// the start of the window. Mapper registers live at 0x08800100..0x0880010F.
//
// The command protocol and reset mappings are hardware-reference behaviour
// documented by mGBA's src/gba/cart/matrix.c (MPL-2.0). This implementation
// is original code shaped for gbarecomp's immutable host ROM buffer.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gbarecomp::debug {
class SnapshotReader;
class SnapshotWriter;
}

namespace gba {

class GbaMatrixMemory {
public:
    static constexpr std::size_t kNormalRomLimit = 32u * 1024u * 1024u;
    static constexpr std::size_t kPageSize = 0x200u;
    static constexpr std::size_t kPageCount = 16u;
    static constexpr std::size_t kApertureSize = kPageSize * kPageCount;

    // Matrix carts are identified the same way as the hardware-reference
    // implementation: an image larger than the normal GBA window whose game
    // code begins with 'M' at header offset 0xAC.
    void configure(const uint8_t* rom, std::size_t rom_size);

    bool active() const { return active_; }

    // Translate a canonical 0..0x01FFFFFF GBA ROM offset into the physical
    // cartridge image. Offsets outside the mapper aperture pass through.
    // Returns false if the resulting physical access would exceed the image.
    bool translate(std::size_t virtual_offset, std::size_t width,
                   std::size_t* physical_offset) const;

    // True when addr selects the mapper register block in any ROM wait-state
    // mirror. Byte writes are not part of the Matrix protocol.
    static bool is_register_address(uint32_t addr);
    void write16(uint32_t addr, uint16_t value);
    void write32(uint32_t addr, uint32_t value);

    uint32_t command() const { return command_; }
    uint32_t physical_address() const { return physical_address_; }
    uint32_t virtual_address() const { return virtual_address_; }
    uint32_t transfer_size() const { return transfer_size_; }
    uint32_t map_command_count() const { return map_command_count_; }
    uint32_t highest_physical_end() const { return highest_physical_end_; }
    const std::array<uint32_t, kPageCount>& mappings() const {
        return mappings_;
    }

    void serialize(gbarecomp::debug::SnapshotWriter& w) const;
    void deserialize(gbarecomp::debug::SnapshotReader& r);

private:
    void reset();
    void write_register(uint32_t reg, uint32_t value);
    bool remap();

    bool active_ = false;
    std::size_t rom_size_ = 0;
    uint32_t command_ = 0;
    uint32_t physical_address_ = 0;
    uint32_t virtual_address_ = 0;
    uint32_t transfer_size_ = 0;
    uint32_t map_command_count_ = 0;
    uint32_t highest_physical_end_ = 0;
    std::array<uint32_t, kPageCount> mappings_{};
};

}  // namespace gba
