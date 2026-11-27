#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "../debug/mod_state.h"
#include "../armv4t/native_program.h"

namespace gbarecomp {

// Physical offsets in the loaded image whose bytes determine native code.
using CartridgeNativeRange = RuntimeNativeImageRange;

// One offline session owns the image and the resolved write footprint. The
// original disk image is never a destination. All allocation happens at bind;
// snapshot preflight is read-only and restore is allocation-free.
class CartridgeModState {
public:
    using Identity = std::array<std::uint8_t, 32>;
    CartridgeModState() = default;
    CartridgeModState(const CartridgeModState&) = delete;
    CartridgeModState& operator=(const CartridgeModState&) = delete;

    bool bind(std::span<std::uint8_t> image,
              std::span<const std::uint32_t> writable_offsets,
              std::span<const CartridgeNativeRange> native_ranges,
              bool native_map_complete, Identity target, Identity plan,
              std::string* error = nullptr);
    bool read(std::uint64_t offset, std::span<std::uint8_t> out) const noexcept;
    bool can_write(std::uint64_t offset, std::span<const std::uint8_t> bytes,
                   std::string* error = nullptr) const;
    bool write(std::uint64_t offset,
               std::span<const std::uint8_t> bytes) noexcept;
    debug::ModStateProvider snapshot_provider();
    std::size_t footprint() const { return slots_.size(); }

private:
    struct Slot {
        std::uint32_t offset;
        std::uint8_t original;
        bool native;
    };
    const Slot* slot(std::uint32_t offset) const noexcept;
    bool save(debug::SnapshotWriter&, std::string*) const;
    bool preflight(debug::SnapshotReader&, std::string*) const;
    void restore(debug::SnapshotReader&) noexcept;

    std::span<std::uint8_t> image_;
    std::vector<Slot> slots_;
    Identity target_{}, plan_{};
    bool bound_ = false;
};
} // namespace gbarecomp
