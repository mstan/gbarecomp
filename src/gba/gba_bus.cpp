// gba_bus.cpp — see gba_bus.h.
//
// Region dispatch follows `gba_memory.h::classify()` and uses the
// canonical mirror behavior from `resolve_offset()`. The hot loop
// reads/writes directly into the backing array for each region;
// BIOS reads go through `GbaBios::read*()` so the loader can refuse
// to serve bytes before the hash is verified.

#include "gba_bus.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "snapshot.h"

namespace gba {

GbaBus::GbaBus() {
    io_dispatch_.set_audio(&audio_);
}
GbaBus::~GbaBus() = default;

void GbaBus::serialize(gbarecomp::debug::SnapshotWriter& w) const {
    w.bytes(ewram_.data(), ewram_.size());
    w.bytes(iwram_.data(), iwram_.size());
    w.bytes(pal_.data(),   pal_.size());
    w.bytes(vram_.data(),  vram_.size());
    w.bytes(oam_.data(),   oam_.size());
    w.u32(last_fetched_);
    w.boolean(bios_access_enabled_);
    w.u64(unmapped_count_);
}

void GbaBus::deserialize(gbarecomp::debug::SnapshotReader& r) {
    r.bytes(ewram_.data(), ewram_.size());
    r.bytes(iwram_.data(), iwram_.size());
    r.bytes(pal_.data(),   pal_.size());
    r.bytes(vram_.data(),  vram_.size());
    r.bytes(oam_.data(),   oam_.size());
    last_fetched_        = r.u32();
    bios_access_enabled_ = r.boolean();
    unmapped_count_      = static_cast<std::size_t>(r.u64());
}

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

bool is_eeprom_addr(uint32_t addr, const GbaSave& save) {
    return save.eeprom_enabled() && ((addr >> 24) == 0x0Du);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Reads
// ─────────────────────────────────────────────────────────────────────

// GBA open-bus: the value a protected-BIOS / unmapped read returns is the
// recently pre-fetched opcode. We read it straight from the BIOS/ROM image at
// the prefetch slot (ARM: word at PC+8; THUMB: the PC+4 halfword mirrored into
// both halves, per GBATEK). The prefetch source is the same image the CPU is
// fetching from — never recurse through the bus accessors.
uint32_t GbaBus::prefetch_word(uint32_t pc, bool thumb) const {
    auto code32 = [&](uint32_t a) -> uint32_t {
        if (a < 0x00004000u) return bios_ ? bios_->read32(a & 0x3FFFu) : 0u;
        if (rom_ && a >= 0x08000000u) {
            uint32_t o = a - 0x08000000u;
            if (o + 3u < rom_size_) return load_u32(&rom_[o]);
        }
        return 0u;
    };
    auto code16 = [&](uint32_t a) -> uint16_t {
        if (a < 0x00004000u) return bios_ ? bios_->read16(a & 0x3FFFu) : uint16_t{0};
        if (rom_ && a >= 0x08000000u) {
            uint32_t o = a - 0x08000000u;
            if (o + 1u < rom_size_) return load_u16(&rom_[o]);
        }
        return 0u;
    };
    if (thumb) {
        uint32_t h = code16(pc + 4u);
        return h | (h << 16);
    }
    return code32(pc + 8u);
}

uint8_t GbaBus::read8(uint32_t addr) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    switch (region) {
        case Region::Bios:
            if (bios_ && bios_access_enabled_)
                return bios_->read8(static_cast<uint32_t>(off));
            return static_cast<uint8_t>(bios_prefetch_ >> (8u * (addr & 3u)));
        case Region::Ewram: return ewram_[off];
        case Region::Iwram: return iwram_[off];
        case Region::Pal:   return pal_[off];
        case Region::Vram:  if (off < vram_.size()) return vram_[off]; break;
        case Region::Oam:   return oam_[off];
        case Region::Rom: {
            // Cartridge GPIO (RTC) at 0x080000C4..0xC9 when readable; else
            // the bus returns ordinary ROM (write-only GPIO mode).
            if (rtc_.active() && rtc_.read_enabled() && off >= 0xC4u && off <= 0xC9u)
                return rtc_.read(static_cast<uint32_t>(off));
            if (is_eeprom_addr(addr, save_)) {
                return static_cast<uint8_t>(save_.eeprom_read_bit());
            }
            if (rom_ && off < rom_size_) return rom_[off];
            // No-cart open-bus: ROM reads return the cart-address-bus
            // value (per GBATEK § "GBA Cartridge ROM" — when no cart
            // asserts data, the 16-bit address drives the data lines).
            // read16(0x08000000 + 2N) = N. Byte reads pick the right
            // half. Native must match mGBA here for BIOS-only diff.
            uint32_t halfword_index = static_cast<uint32_t>(off >> 1);
            uint16_t hw = static_cast<uint16_t>(halfword_index & 0xFFFFu);
            return (off & 1) ? static_cast<uint8_t>(hw >> 8)
                             : static_cast<uint8_t>(hw & 0xFFu);
        }
        case Region::Io:
            return io_dispatch_.read8(static_cast<uint32_t>(off));
        case Region::Save:
            log_unmapped(addr, 0, false, 1);
            return 0;
        case Region::OpenBus:
        case Region::Unknown: {
            uint32_t ob = prefetch_word(open_bus_pc_, open_bus_thumb_);
            log_unmapped(addr, ob, false, 1);
            return static_cast<uint8_t>(ob >> (8u * (addr & 3u)));
        }
    }
    return 0;
}

uint16_t GbaBus::read16(uint32_t addr) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    switch (region) {
        case Region::Bios:
            if (bios_ && bios_access_enabled_)
                return bios_->read16(static_cast<uint32_t>(off));
            return static_cast<uint16_t>(bios_prefetch_ >> (8u * (addr & 2u)));
        case Region::Ewram: return load_u16(&ewram_[off]);
        case Region::Iwram: return load_u16(&iwram_[off]);
        case Region::Pal:   return load_u16(&pal_[off]);
        case Region::Vram:
            if (off + 1 < vram_.size()) return load_u16(&vram_[off]);
            break;
        case Region::Oam:   return load_u16(&oam_[off]);
        case Region::Rom: {
            if (rtc_.active() && rtc_.read_enabled() && off >= 0xC4u && off <= 0xC8u) {
                uint32_t o = static_cast<uint32_t>(off);
                return static_cast<uint16_t>(rtc_.read(o) | (rtc_.read(o + 1) << 8));
            }
            if (is_eeprom_addr(addr, save_)) {
                return save_.eeprom_read_bit();
            }
            if (rom_ && off + 1 < rom_size_) return load_u16(&rom_[off]);
            // No-cart open-bus: read16 returns the halfword index.
            return static_cast<uint16_t>((off >> 1) & 0xFFFFu);
        }
        case Region::Io:
            return io_dispatch_.read16(static_cast<uint32_t>(off));
        case Region::Save:
            log_unmapped(addr, 0, false, 2);
            return 0;
        case Region::OpenBus:
        case Region::Unknown: {
            uint32_t ob = prefetch_word(open_bus_pc_, open_bus_thumb_);
            log_unmapped(addr, ob, false, 2);
            return static_cast<uint16_t>(ob >> (8u * (addr & 2u)));
        }
    }
    return 0;
}

uint32_t GbaBus::read32(uint32_t addr) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    uint32_t v = 0;
    switch (region) {
        case Region::Bios:
            if (bios_ && bios_access_enabled_) {
                v = bios_->read32(static_cast<uint32_t>(off));
                last_fetched_ = v;
                return v;
            }
            return bios_prefetch_;
        case Region::Ewram: return load_u32(&ewram_[off]);
        case Region::Iwram: return load_u32(&iwram_[off]);
        case Region::Pal:   return load_u32(&pal_[off]);
        case Region::Vram:
            if (off + 3 < vram_.size()) return load_u32(&vram_[off]);
            break;
        case Region::Oam:   return load_u32(&oam_[off]);
        case Region::Rom: {
            if (rtc_.active() && rtc_.read_enabled() && off >= 0xC4u && off <= 0xC6u) {
                uint32_t o = static_cast<uint32_t>(off);
                return rtc_.read(o) | (rtc_.read(o + 1) << 8) |
                       (rtc_.read(o + 2) << 16) | (rtc_.read(o + 3) << 24);
            }
            if (is_eeprom_addr(addr, save_)) {
                return save_.eeprom_read_bit();
            }
            if (rom_ && off + 3 < rom_size_) return load_u32(&rom_[off]);
            // No-cart open-bus: two consecutive halfwords.
            uint32_t hw_lo = (off >> 1) & 0xFFFFu;
            uint32_t hw_hi = ((off >> 1) + 1) & 0xFFFFu;
            return hw_lo | (hw_hi << 16);
        }
        case Region::Io:
            return io_dispatch_.read32(static_cast<uint32_t>(off));
        case Region::Save:
            log_unmapped(addr, 0, false, 4);
            return 0;
        case Region::OpenBus:
        case Region::Unknown: {
            uint32_t ob = prefetch_word(open_bus_pc_, open_bus_thumb_);
            log_unmapped(addr, ob, false, 4);
            return ob;
        }
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────
// Writes
// ─────────────────────────────────────────────────────────────────────

bool GbaBus::observe_write(Region region, std::size_t off, uint32_t addr,
                           uint8_t width, uint32_t new_value) {
    BusWriteRegion br;
    const uint8_t* p = nullptr;
    std::size_t cap = 0;
    switch (region) {
        case Region::Ewram: br = BusWriteRegion::Ewram; p = ewram_.data(); cap = ewram_.size(); break;
        case Region::Iwram: br = BusWriteRegion::Iwram; p = iwram_.data(); cap = iwram_.size(); break;
        case Region::Pal:   br = BusWriteRegion::Pal;   p = pal_.data();   cap = pal_.size();   break;
        case Region::Vram:  br = BusWriteRegion::Vram;  p = vram_.data();  cap = vram_.size();  break;
        case Region::Oam:   br = BusWriteRegion::Oam;   p = oam_.data();   cap = oam_.size();   break;
        default:            br = BusWriteRegion::Device; break;
    }
    uint32_t old = 0;
    if (p && off + width <= cap) {
        if (width == 1) old = p[off];
        else if (width == 2) old = uint32_t(p[off]) | (uint32_t(p[off + 1]) << 8);
        else old = uint32_t(p[off]) | (uint32_t(p[off + 1]) << 8) |
                   (uint32_t(p[off + 2]) << 16) | (uint32_t(p[off + 3]) << 24);
    }
    return write_observer_->on_bus_write(br, static_cast<uint32_t>(off), addr,
                                         width, old, new_value);
}

void GbaBus::write8(uint32_t addr, uint8_t v) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    if (write_observer_ && observe_write(region, off, addr, 1, v)) return;
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
            if (rtc_.active() && off >= 0xC4u && off <= 0xC9u) {
                rtc_.write(static_cast<uint32_t>(off), v);
                return;
            }
            if (is_eeprom_addr(addr, save_)) {
                save_.eeprom_write_bit(v);
                return;
            }
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
    if (write_observer_ && observe_write(region, off, addr, 2, v)) return;
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
            if (region == Region::Rom && rtc_.active() && off >= 0xC4u && off <= 0xC8u) {
                rtc_.write(static_cast<uint32_t>(off), static_cast<uint8_t>(v & 0xFF));
                return;
            }
            if (region == Region::Rom && is_eeprom_addr(addr, save_)) {
                save_.eeprom_write_bit(v);
                return;
            }
            log_unmapped(addr, v, true, 2);
            return;
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
    if (write_observer_ && observe_write(region, off, addr, 4, v)) return;
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
            if (region == Region::Rom && rtc_.active() && off >= 0xC4u && off <= 0xC6u) {
                rtc_.write(static_cast<uint32_t>(off), static_cast<uint8_t>(v & 0xFF));
                return;
            }
            if (region == Region::Rom && is_eeprom_addr(addr, save_)) {
                save_.eeprom_write_bit(static_cast<uint16_t>(v));
                return;
            }
            log_unmapped(addr, v, true, 4);
            return;
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

// Per-region access-cycle table. Sources:
//   * GBATEK § "GBA Memory Map"
//   * GBATEK § "GBA Cycle Times"
//   * ARM7TDMI TRM § "Memory Access Cycles"
//
// Cycle counts assume default WAITCNT (0x0000). Bus width and
// waitstate combine: every 16-bit-bus region costs 2 cycles per
// 32-bit access (two consecutive halfword bus cycles); EWRAM adds
// 2 waitstates on top of that.
//
// `sequential` is the ARM7TDMI "S" cycle (an access immediately
// following another in the same area). For ARM/THUMB on the GBA
// most regions have S=N=1 for 16-bit and 1S=1N=1 / 2S=2N=2 for
// 32-bit, but ROM with WAITCNT > 0 gets a faster S than N. We use
// the same value for S/N in regions where they match and split
// only where they don't (ROM).
uint32_t GbaBus::access_cycles(uint32_t addr, uint8_t width,
                               bool sequential) const {
    // 8-bit accesses use the same cycle count as 16-bit on hardware
    // (regions with 16-bit data buses still complete a single
    // halfword cycle for either width).
    uint8_t w = (width == 4) ? 4 : 2;
    uint32_t region = (addr >> 24) & 0xFu;
    switch (region) {
        case 0x0:  // BIOS  (32-bit bus, 0 wait)
        case 0x3:  // IWRAM (32-bit bus, 0 wait)
        case 0x7:  // OAM   (32-bit bus, 0 wait)
            return 1;
        case 0x4:  // IO (32-bit bus, 0 wait)
            return 1;
        case 0x5:  // PAL   (16-bit bus, 0 wait)
        case 0x6:  // VRAM  (16-bit bus, 0 wait)
            return (w == 4) ? 2u : 1u;
        case 0x2:  // EWRAM (16-bit bus, 2 wait states)
            return (w == 4) ? 6u : 3u;
        case 0x8: case 0x9:  // ROM WS0
        case 0xA: case 0xB:  // ROM WS1
        case 0xC: case 0xD:  // ROM WS2
            // Default WAITCNT 0x0000. mGBA's waitstate tables store
            // the external wait component (N16=4, S16=2, N32=7,
            // S32=5); the data-access helper needs the full bus
            // access cost, so add the one base bus cycle here.
            if (w == 4) return sequential ? 6u : 8u;
            return sequential ? 3u : 5u;
        case 0xE:  // SRAM / Flash region — 8-bit bus, ~5 cycles
            return 5;
        default:
            return 1;
    }
}

void GbaBus::log_unmapped(uint32_t addr, uint32_t value, bool is_write, uint8_t width) {
    ++unmapped_count_;
    // Keep the counter hot-path cheap by default. Full stderr logging is
    // useful when chasing a specific bus issue, but gameplay can produce
    // millions of open-bus-style reads and that makes TCP replays unusable.
    if (!std::getenv("GBARECOMP_LOG_UNMAPPED")) {
        return;
    }
    std::fprintf(stderr,
                 "[gba:bus] UNMAPPED %s%u @ 0x%08x = 0x%x\n",
                 is_write ? "W" : "R", width, addr, value);
}

}  // namespace gba
