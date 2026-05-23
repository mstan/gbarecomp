// gba_memory.cpp — memory map classification + mirror resolution.
//
// This file is intentionally small and reference-implementation style.
// The hot bus path (gba_bus.cpp) consults `classify` and `resolve_offset`
// once and caches per-region pointers; codegen-emitted loads / stores
// go through the cached pointer except for IO and ROM-with-waitstate
// regions.

#include "gba_memory.h"

namespace gba {

const char* region_name(Region r) noexcept {
    switch (r) {
        case Region::Bios:    return "BIOS";
        case Region::Ewram:   return "EWRAM";
        case Region::Iwram:   return "IWRAM";
        case Region::Io:      return "IO";
        case Region::Pal:     return "PAL";
        case Region::Vram:    return "VRAM";
        case Region::Oam:     return "OAM";
        case Region::Rom:     return "ROM";
        case Region::Save:    return "SAVE";
        case Region::OpenBus: return "OPENBUS";
        case Region::Unknown: return "UNKNOWN";
    }
    return "??";
}

Region classify(uint32_t addr) noexcept {
    // Top-nibble fast path. The GBA address space is sparse; everything
    // above 0x0F000000 is wholly unmapped.
    uint32_t top = addr >> 24;
    switch (top) {
        case 0x00: return (addr < kBiosBase + kBiosSize) ? Region::Bios
                                                         : Region::OpenBus;
        case 0x02: return Region::Ewram;
        case 0x03: return Region::Iwram;
        case 0x04: return Region::Io;
        case 0x05: return Region::Pal;
        case 0x06: return Region::Vram;
        case 0x07: return Region::Oam;
        case 0x08: case 0x09:
        case 0x0A: case 0x0B:
        case 0x0C: case 0x0D:
            return Region::Rom;
        case 0x0E: case 0x0F:
            return Region::Save;
        default:
            return Region::OpenBus;
    }
}

std::size_t resolve_offset(uint32_t addr, Region region) noexcept {
    switch (region) {
        case Region::Bios:
            return static_cast<std::size_t>(addr - kBiosBase);
        case Region::Ewram:
            // EWRAM mirrors every 256 KB across 0x02000000..0x02FFFFFF.
            return static_cast<std::size_t>(addr & (kEwramSize - 1));
        case Region::Iwram:
            // IWRAM mirrors every 32 KB across 0x03000000..0x03FFFFFF.
            return static_cast<std::size_t>(addr & (kIwramSize - 1));
        case Region::Io:
            // IO range is 0x04000000..0x040003FF; addresses above this
            // are partial-decode and the bus must log them. We return
            // a sentinel offset that the bus treats as unmapped.
            return static_cast<std::size_t>(addr - kIoBase);
        case Region::Pal:
            return static_cast<std::size_t>(addr & (kPalSize - 1));
        case Region::Vram: {
            // VRAM is 96 KB physical mapped into a 128 KB window. The
            // upper 32 KB mirrors the obj tile area. The exact layout
            // is implemented in gba_ppu.cpp; here we return the linear
            // 128 KB offset and let the PPU bus do the fold-down.
            return static_cast<std::size_t>(addr & 0x1FFFFu);
        }
        case Region::Oam:
            return static_cast<std::size_t>(addr & (kOamSize - 1));
        case Region::Rom:
            // ROM mirrors the same physical contents three times
            // (waitstate 0/1/2). Caller knows which waitstate slot was
            // used; we just normalize to the ROM offset.
            return static_cast<std::size_t>(addr & 0x01FFFFFFu);
        case Region::Save:
            // Save offset within the cartridge save region. The actual
            // mapping depends on the detected save chip; the save
            // controller in gba_save.cpp owns that.
            return static_cast<std::size_t>(addr - kSaveBase);
        case Region::OpenBus:
        case Region::Unknown:
            return 0;
    }
    return 0;
}

}  // namespace gba
