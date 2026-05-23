// gba_bus.cpp — see gba_bus.h.
//
// Region dispatch follows `gba_memory.h::classify()` and uses the
// canonical mirror behavior from `resolve_offset()`. The hot loop
// reads/writes directly into the backing array for each region;
// BIOS reads go through `GbaBios::read*()` so the loader can refuse
// to serve bytes before the hash is verified.

#include "gba_bus.h"

#include <cstdio>
#include <cstring>

namespace gba {

GbaBus::GbaBus()  = default;
GbaBus::~GbaBus() = default;

namespace {

// Helper: read a little-endian halfword/word from a byte buffer at
// the given offset. Caller has already bounds-checked.
uint16_t load_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t load_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) |
                                 (p[2] << 16) | (p[3] << 24));
}
void store_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void store_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Reads
// ─────────────────────────────────────────────────────────────────────

uint8_t GbaBus::read8(uint32_t addr) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    switch (region) {
        case Region::Bios:
            return bios_ ? bios_->read8(static_cast<uint32_t>(off)) : 0;
        case Region::Ewram: return ewram_[off];
        case Region::Iwram: return iwram_[off];
        case Region::Pal:   return pal_[off];
        case Region::Vram:  if (off < vram_.size()) return vram_[off]; break;
        case Region::Oam:   return oam_[off];
        case Region::Rom:
            return (rom_ && off < rom_size_) ? rom_[off] : 0;
        case Region::Io:
            return io_dispatch_.read8(static_cast<uint32_t>(off));
        case Region::Save:
        case Region::OpenBus:
        case Region::Unknown:
            log_unmapped(addr, 0, false, 1);
            return 0;
    }
    return 0;
}

uint16_t GbaBus::read16(uint32_t addr) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    switch (region) {
        case Region::Bios:
            return bios_ ? bios_->read16(static_cast<uint32_t>(off)) : 0;
        case Region::Ewram: return load_u16(&ewram_[off]);
        case Region::Iwram: return load_u16(&iwram_[off]);
        case Region::Pal:   return load_u16(&pal_[off]);
        case Region::Vram:
            if (off + 1 < vram_.size()) return load_u16(&vram_[off]);
            break;
        case Region::Oam:   return load_u16(&oam_[off]);
        case Region::Rom:
            return (rom_ && off + 1 < rom_size_)
                ? load_u16(&rom_[off]) : 0;
        case Region::Io:
            return io_dispatch_.read16(static_cast<uint32_t>(off));
        case Region::Save:
        case Region::OpenBus:
        case Region::Unknown:
            log_unmapped(addr, 0, false, 2);
            return 0;
    }
    return 0;
}

uint32_t GbaBus::read32(uint32_t addr) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    uint32_t v = 0;
    switch (region) {
        case Region::Bios:
            v = bios_ ? bios_->read32(static_cast<uint32_t>(off)) : 0;
            last_fetched_ = v;
            return v;
        case Region::Ewram: return load_u32(&ewram_[off]);
        case Region::Iwram: return load_u32(&iwram_[off]);
        case Region::Pal:   return load_u32(&pal_[off]);
        case Region::Vram:
            if (off + 3 < vram_.size()) return load_u32(&vram_[off]);
            break;
        case Region::Oam:   return load_u32(&oam_[off]);
        case Region::Rom:
            return (rom_ && off + 3 < rom_size_)
                ? load_u32(&rom_[off]) : 0;
        case Region::Io:
            return io_dispatch_.read32(static_cast<uint32_t>(off));
        case Region::Save:
        case Region::OpenBus:
        case Region::Unknown:
            log_unmapped(addr, 0, false, 4);
            return 0;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────
// Writes
// ─────────────────────────────────────────────────────────────────────

void GbaBus::write8(uint32_t addr, uint8_t v) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    switch (region) {
        case Region::Ewram: ewram_[off] = v; return;
        case Region::Iwram: iwram_[off] = v; return;
        case Region::Pal:   pal_[off]   = v; return;
        case Region::Vram:
            if (off < vram_.size()) vram_[off] = v;
            return;
        case Region::Oam:   oam_[off] = v; return;
        case Region::Bios:
            // BIOS is read-only. A write here is suspicious; trace it.
            log_unmapped(addr, v, true, 1);
            return;
        case Region::Rom:
            // Cartridge ROM is read-only at write time (writes to ROM
            // are used by some save-chip protocols, but that's the
            // SAVE region, not the ROM region itself).
            log_unmapped(addr, v, true, 1);
            return;
        case Region::Io:
            io_dispatch_.write8(static_cast<uint32_t>(off), v);
            return;
        case Region::Save:
        case Region::OpenBus:
        case Region::Unknown:
            log_unmapped(addr, v, true, 1);
            return;
    }
}

void GbaBus::write16(uint32_t addr, uint16_t v) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    switch (region) {
        case Region::Ewram: store_u16(&ewram_[off], v); return;
        case Region::Iwram: store_u16(&iwram_[off], v); return;
        case Region::Pal:   store_u16(&pal_[off], v);   return;
        case Region::Vram:
            if (off + 1 < vram_.size()) store_u16(&vram_[off], v);
            return;
        case Region::Oam:   store_u16(&oam_[off], v); return;
        case Region::Bios:
        case Region::Rom:
        case Region::Io:
            io_dispatch_.write16(static_cast<uint32_t>(off), v);
            return;
        case Region::Save:
        case Region::OpenBus:
        case Region::Unknown:
            log_unmapped(addr, v, true, 2);
            return;
    }
}

void GbaBus::write32(uint32_t addr, uint32_t v) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    switch (region) {
        case Region::Ewram: store_u32(&ewram_[off], v); return;
        case Region::Iwram: store_u32(&iwram_[off], v); return;
        case Region::Pal:   store_u32(&pal_[off], v);   return;
        case Region::Vram:
            if (off + 3 < vram_.size()) store_u32(&vram_[off], v);
            return;
        case Region::Oam:   store_u32(&oam_[off], v); return;
        case Region::Bios:
        case Region::Rom:
        case Region::Io:
            io_dispatch_.write32(static_cast<uint32_t>(off), v);
            return;
        case Region::Save:
        case Region::OpenBus:
        case Region::Unknown:
            log_unmapped(addr, v, true, 4);
            return;
    }
}

void GbaBus::log_unmapped(uint32_t addr, uint32_t value, bool is_write, uint8_t width) {
    ++unmapped_count_;
    // Phase 2 will route this to the always-on TraceRing. For now,
    // emit a structured stderr line so it can't be confused with
    // anything else.
    std::fprintf(stderr,
                 "[gba:bus] UNMAPPED %s%u @ 0x%08x = 0x%x\n",
                 is_write ? "W" : "R", width, addr, value);
}

}  // namespace gba
