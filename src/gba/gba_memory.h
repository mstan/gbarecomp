// gba_memory.h — GBA memory map and region descriptors.
//
// Layout per GBATEK § "GBA Memory Map":
//
//   0x00000000  System ROM (BIOS)        16 KB     read-only, executable
//   0x02000000  EWRAM (on-board)         256 KB    R/W, slow (waitstates)
//   0x03000000  IWRAM (on-chip)          32 KB     R/W, fast
//   0x04000000  IO Registers             1 KB      mapped IO
//   0x05000000  PAL RAM (palette)        1 KB      16-bit access
//   0x06000000  VRAM                     96 KB     mode-dependent layout
//   0x07000000  OAM                      1 KB      sprite attribute table
//   0x08000000  Game Pak ROM (wait 0)    up to 32 MB
//   0x0A000000  Game Pak ROM (wait 1)    same ROM, different waitstates
//   0x0C000000  Game Pak ROM (wait 2)    same ROM, different waitstates
//   0x0E000000  Game Pak SRAM/Flash      max 64 KB (Flash 128KB uses banking)
//
// Mirroring behavior is real and visible to games: e.g. IWRAM mirrors
// every 32 KB inside 0x03000000..0x03FFFFFF. The decoder/runtime must
// match hardware, not "nearest convenient" assumption.
//
// Reference: GBATEK § "GBA Memory Map", § "GBA Memory Mirrors".

#pragma once

#include <cstdint>
#include <cstddef>

#include "bus.h"   // armv4t::Bus

namespace gba {

// Region bases
inline constexpr uint32_t kBiosBase     = 0x00000000;
inline constexpr uint32_t kEwramBase    = 0x02000000;
inline constexpr uint32_t kIwramBase    = 0x03000000;
inline constexpr uint32_t kIoBase       = 0x04000000;
inline constexpr uint32_t kPalBase      = 0x05000000;
inline constexpr uint32_t kVramBase     = 0x06000000;
inline constexpr uint32_t kOamBase      = 0x07000000;
inline constexpr uint32_t kRomBase0     = 0x08000000;  // waitstate 0
inline constexpr uint32_t kRomBase1     = 0x0A000000;  // waitstate 1
inline constexpr uint32_t kRomBase2     = 0x0C000000;  // waitstate 2
inline constexpr uint32_t kSaveBase     = 0x0E000000;

// Region sizes
inline constexpr std::size_t kBiosSize  = 16 * 1024;
inline constexpr std::size_t kEwramSize = 256 * 1024;
inline constexpr std::size_t kIwramSize = 32 * 1024;
inline constexpr std::size_t kIoSize    = 0x400;        // documented IO range
inline constexpr std::size_t kPalSize   = 1024;
inline constexpr std::size_t kVramSize  = 96 * 1024;
inline constexpr std::size_t kOamSize   = 1024;
inline constexpr std::size_t kRomMaxSize  = 32 * 1024 * 1024;
inline constexpr std::size_t kSaveMaxSize = 128 * 1024;  // largest supported (Flash 1M)

enum class Region : uint8_t {
    Bios,
    Ewram,
    Iwram,
    Io,
    Pal,
    Vram,
    Oam,
    Rom,
    Save,
    OpenBus,   // unmapped — read returns last-fetched instruction word
    Unknown,
};

const char* region_name(Region r) noexcept;

// Classify an address into a region (without mirroring resolution).
Region classify(uint32_t addr) noexcept;

// Resolve mirroring within a region. Returns the offset into the
// region's backing storage. For OpenBus / Unknown, returns 0 and the
// caller is expected to handle the unmapped path.
//
// NOTE: VRAM mirroring is non-trivial (the 32K obj tile area mirrors
// inside the 96K layout). This helper returns the canonical offset;
// the actual VRAM access path lives in gba_ppu.cpp.
std::size_t resolve_offset(uint32_t addr, Region region) noexcept;

// ─────────────────────────────────────────────────────────────────────
// MemoryBus — the bus interface generated code talks to.
//
// Concrete implementations live in gba_bus.cpp; this header just
// declares the read/write API so armv4t codegen sees a stable shape.
// ─────────────────────────────────────────────────────────────────────
// Inherits the portable read/write primitives from armv4t::Bus and
// adds GBA-specific hooks (unmapped-access logging). Generated code
// and the interpreter take `armv4t::Bus&` and don't see the GBA
// extension; the runtime sees the full MemoryBus.
struct MemoryBus : public armv4t::Bus {
    // Structured trace hook for an unmapped or partial-decode IO access.
    // Implementations MUST emit a record to the always-on ring, not
    // silently return a magic value.
    virtual void log_unmapped(uint32_t addr,
                              uint32_t value,
                              bool is_write,
                              uint8_t  width) = 0;
};

}  // namespace gba
