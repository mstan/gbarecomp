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

extern "C" int (*g_rom_read16_override)(std::uint32_t, std::uint16_t,
                                         std::uint16_t*) = nullptr;
extern "C" int (*g_rom_read32_override)(std::uint32_t, std::uint32_t,
                                         std::uint32_t*) = nullptr;

namespace {
// The bus whose wait-state table currently lives in the runtime's live copy
// (at most one). Cleared by that bus's destructor, so the runtime never
// unbinds a destroyed bus.
GbaBus* g_wait_table_owner = nullptr;
}  // namespace

GbaBus::GbaBus() {
    io_dispatch_.set_audio(&audio_);
    waits_reset(*waits_);
}
GbaBus::~GbaBus() {
    if (g_wait_table_owner == this) g_wait_table_owner = nullptr;
}

void GbaBus::serialize(gbarecomp::debug::SnapshotWriter& w) const {
    w.bytes(ewram_.data(), ewram_.size());
    w.bytes(iwram_.data(), iwram_.size());
    w.bytes(pal_.data(),   pal_.size());
    w.bytes(vram_.data(),  vram_.size());
    w.bytes(oam_.data(),   oam_.size());
    w.u32(last_fetched_);
    w.boolean(bios_access_enabled_);
    w.u64(unmapped_count_);
    rtc_.serialize(w);
    matrix_.serialize(w);
    // Wait states: WAITCNT itself lives in the IO page; the EWRAM wait is
    // the last ACCEPTED memory-control setting (a rejected write keeps it).
    w.u32(memctl_);
    w.u32(waits_->n16[0x2]);
    w.u32(waits_->last_prefetched_pc);
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
    if (r.remaining() != 0) {
        rtc_.deserialize(r);
    }
    if (r.remaining() != 0) {
        matrix_.deserialize(r);
    }
    // Older snapshots predate the wait-state model: power-on EWRAM control
    // and an empty prefetch buffer. WAITCNT is re-derived after the IO page
    // loads (refresh_waitstates, called by the snapshot orchestrator).
    waits_reset(*waits_);
    memctl_ = 0x0D000020u;
    if (r.remaining() != 0) {
        memctl_ = r.u32();
        const uint32_t ewram_wait = r.u32();
        if (ewram_wait >= 1u && ewram_wait <= 15u)
            waits_->n16[0x2] = static_cast<uint8_t>(ewram_wait);
        waits_->last_prefetched_pc = r.u32();
    }
    refresh_waitstates();
}

GbaBus* GbaBus::wait_table_owner() { return g_wait_table_owner; }

void GbaBus::bind_wait_table(RuntimeWaitTable* live) {
    if (live) {
        if (g_wait_table_owner && g_wait_table_owner != this)
            g_wait_table_owner->bind_wait_table(nullptr);
        if (waits_ != live) {
            *live = *waits_;
            waits_ = live;
        }
        g_wait_table_owner = this;
        return;
    }
    if (waits_ != &own_waits_) {
        own_waits_ = *waits_;
        waits_ = &own_waits_;
    }
    if (g_wait_table_owner == this) g_wait_table_owner = nullptr;
}

void GbaBus::refresh_waitstates() {
    const uint8_t* io = io_dispatch_.raw();
    waits_apply_waitcnt(*waits_, static_cast<uint16_t>(io[0x204] | (io[0x205] << 8)));
    // EWRAM entries follow the accepted wait (n16[2]; 1..15 by construction).
    waits_apply_memctl(*waits_, (15u - (waits_->n16[0x2] & 0xFu)) << 24);
}

namespace {
bool is_memctl(std::size_t off) {
    const std::size_t o = off & 0xFFFFu;
    return o >= 0x800u && o < 0x804u;
}
}  // namespace

uint32_t GbaBus::memctl_read(uint32_t off, uint8_t width) const {
    const uint32_t shift = 8u * (off & 3u);
    const uint32_t v = memctl_ >> shift;
    return width == 1 ? (v & 0xFFu) : width == 2 ? (v & 0xFFFFu) : v;
}

void GbaBus::memctl_write(uint32_t off, uint32_t value, uint8_t width) {
    const uint32_t shift = 8u * (off & 3u);
    const uint32_t mask = (width == 1 ? 0xFFu : width == 2 ? 0xFFFFu
                                                           : 0xFFFFFFFFu) << shift;
    memctl_ = (memctl_ & ~mask) | ((value << shift) & mask);
    if (mask & 0xFF000000u) waits_apply_memctl(*waits_, memctl_);
}

// WAITCNT (4000204h) is the only IO register the bus itself interprets.
// Bits 13 and 15 are read-only zero on a GBA cartridge (mGBA io.c masks
// writes with 0x5FFF).
void GbaBus::note_io_write(uint32_t off, uint8_t width) {
    if (off + width <= 0x204u || off > 0x205u) return;
    const uint8_t* io = io_dispatch_.raw();
    const uint16_t raw = static_cast<uint16_t>(io[0x204] | (io[0x205] << 8));
    const uint16_t masked = static_cast<uint16_t>(raw & 0x5FFFu);
    if (masked != raw) io_dispatch_.write16(0x204u, masked);
    waits_apply_waitcnt(*waits_, masked);
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
        if (rom_ && a >= 0x08000000u && a < 0x0E000000u) {
            std::size_t o = (a - 0x08000000u) & 0x01FFFFFFu;
            std::size_t physical = 0;
            if (matrix_.translate(o, 4, &physical))
                return load_u32(&rom_[physical]);
        }
        return 0u;
    };
    auto code16 = [&](uint32_t a) -> uint16_t {
        if (a < 0x00004000u) return bios_ ? bios_->read16(a & 0x3FFFu) : uint16_t{0};
        if (rom_ && a >= 0x08000000u && a < 0x0E000000u) {
            std::size_t o = (a - 0x08000000u) & 0x01FFFFFFu;
            std::size_t physical = 0;
            if (matrix_.translate(o, 2, &physical))
                return load_u16(&rom_[physical]);
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
            if (gpio_.active() && gpio_.read_enabled() && off >= 0xC4u && off <= 0xC9u)
                return gpio_.read(static_cast<uint32_t>(off));
            if (is_eeprom_addr(addr, save_)) {
                return static_cast<uint8_t>(save_.eeprom_read_bit());
            }
            std::size_t physical = 0;
            if (rom_ && matrix_.translate(off, 1, &physical))
                return rom_[physical];
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
            if (write_observer_) write_observer_->on_bus_read(addr);
            if (is_memctl(off))
                return static_cast<uint8_t>(memctl_read(static_cast<uint32_t>(off), 1));
            return io_dispatch_.read8(static_cast<uint32_t>(off));
        case Region::Save:
            if (save_.sram_enabled())
                return save_.sram_read(static_cast<uint32_t>(off));
            if (save_.flash_enabled())
                return save_.flash_read(static_cast<uint32_t>(off));
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
            if (gpio_.active() && gpio_.read_enabled() && off >= 0xC4u && off <= 0xC8u) {
                uint32_t o = static_cast<uint32_t>(off);
                return static_cast<uint16_t>(gpio_.read(o) | (gpio_.read(o + 1) << 8));
            }
            if (is_eeprom_addr(addr, save_)) {
                return save_.eeprom_read_bit();
            }
            std::size_t physical = 0;
            if (rom_ && matrix_.translate(off, 2, &physical)) {
                const uint16_t original = load_u16(&rom_[physical]);
                uint16_t overridden = original;
                if (g_rom_read16_override &&
                    g_rom_read16_override(addr, original, &overridden)) {
                    return overridden;
                }
                return original;
            }
            // No-cart open-bus: read16 returns the halfword index.
            return static_cast<uint16_t>((off >> 1) & 0xFFFFu);
        }
        case Region::Io:
            if (write_observer_) write_observer_->on_bus_read(addr);
            if (is_memctl(off))
                return static_cast<uint16_t>(memctl_read(static_cast<uint32_t>(off), 2));
            return io_dispatch_.read16(static_cast<uint32_t>(off));
        case Region::Save:
            if (save_.sram_enabled()) {
                uint8_t b = save_.sram_read(static_cast<uint32_t>(off));
                return static_cast<uint16_t>(b | (b << 8));  // 8-bit bus mirror
            }
            if (save_.flash_enabled()) {
                uint8_t b = save_.flash_read(static_cast<uint32_t>(off));
                return static_cast<uint16_t>(b | (b << 8));  // 8-bit bus mirror
            }
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
            if (gpio_.active() && gpio_.read_enabled() && off >= 0xC4u && off <= 0xC6u) {
                uint32_t o = static_cast<uint32_t>(off);
                return gpio_.read(o) | (gpio_.read(o + 1) << 8) |
                       (gpio_.read(o + 2) << 16) | (gpio_.read(o + 3) << 24);
            }
            if (is_eeprom_addr(addr, save_)) {
                return save_.eeprom_read_bit();
            }
            std::size_t physical = 0;
            if (rom_ && matrix_.translate(off, 4, &physical)) {
                const uint32_t original = load_u32(&rom_[physical]);
                uint32_t overridden = original;
                if (g_rom_read32_override &&
                    g_rom_read32_override(addr, original, &overridden)) {
                    return overridden;
                }
                return original;
            }
            // No-cart open-bus: two consecutive halfwords.
            uint32_t hw_lo = (off >> 1) & 0xFFFFu;
            uint32_t hw_hi = ((off >> 1) + 1) & 0xFFFFu;
            return hw_lo | (hw_hi << 16);
        }
        case Region::Io:
            if (write_observer_) write_observer_->on_bus_read(addr);
            if (is_memctl(off)) return memctl_read(static_cast<uint32_t>(off), 4);
            return io_dispatch_.read32(static_cast<uint32_t>(off));
        case Region::Save:
            if (save_.sram_enabled()) {
                uint8_t b = save_.sram_read(static_cast<uint32_t>(off));
                return static_cast<uint32_t>(b) * 0x01010101u;  // 8-bit bus mirror
            }
            if (save_.flash_enabled()) {
                uint8_t b = save_.flash_read(static_cast<uint32_t>(off));
                return static_cast<uint32_t>(b) * 0x01010101u;  // 8-bit bus mirror
            }
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
            // BIOS is read-only; hardware ignores writes to this window.
            return;
        case Region::Rom:
            if (matrix_.active() &&
                GbaMatrixMemory::is_register_address(addr)) {
                // Matrix Memory does not define an 8-bit command path.
                log_unmapped(addr, v, true, 1);
                return;
            }
            if (gpio_.active() && off >= 0xC4u && off <= 0xC9u) {
                gpio_.write(static_cast<uint32_t>(off), v);
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
            if (is_memctl(off)) {
                memctl_write(static_cast<uint32_t>(off), v, 1);
                return;
            }
            io_dispatch_.write8(static_cast<uint32_t>(off), v);
            note_io_write(static_cast<uint32_t>(off), 1);
            return;
        case Region::Save:
            if (save_.sram_enabled()) {
                save_.sram_write(static_cast<uint32_t>(off), v);
                return;
            }
            if (save_.flash_enabled()) {
                save_.flash_write(static_cast<uint32_t>(off), v);
                return;
            }
            log_unmapped(addr, v, true, 1);
            return;
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
            // BIOS is read-only; hardware ignores writes to this window.
            return;
        case Region::Rom:
            if (matrix_.active() &&
                GbaMatrixMemory::is_register_address(addr)) {
                matrix_.write16(addr, v);
                return;
            }
            if (region == Region::Rom && gpio_.active() && off >= 0xC4u && off <= 0xC8u) {
                gpio_.write(static_cast<uint32_t>(off), static_cast<uint8_t>(v & 0xFF));
                return;
            }
            if (region == Region::Rom && is_eeprom_addr(addr, save_)) {
                save_.eeprom_write_bit(v);
                return;
            }
            log_unmapped(addr, v, true, 2);
            return;
        case Region::Io:
            if (is_memctl(off)) {
                memctl_write(static_cast<uint32_t>(off), v, 2);
                return;
            }
            io_dispatch_.write16(static_cast<uint32_t>(off), v);
            note_io_write(static_cast<uint32_t>(off), 2);
            return;
        case Region::Save:
            if (save_.sram_enabled()) {
                save_.sram_write(static_cast<uint32_t>(off),
                                 static_cast<uint8_t>(v & 0xFF));
                return;
            }
            if (save_.flash_enabled()) {
                save_.flash_write(static_cast<uint32_t>(off),
                                  static_cast<uint8_t>(v & 0xFF));
                return;
            }
            log_unmapped(addr, v, true, 2);
            return;
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
            // BIOS is read-only; hardware ignores writes to this window.
            return;
        case Region::Rom:
            if (matrix_.active() &&
                GbaMatrixMemory::is_register_address(addr)) {
                matrix_.write32(addr, v);
                return;
            }
            if (region == Region::Rom && gpio_.active() && off >= 0xC4u && off <= 0xC6u) {
                gpio_.write(static_cast<uint32_t>(off), static_cast<uint8_t>(v & 0xFF));
                return;
            }
            if (region == Region::Rom && is_eeprom_addr(addr, save_)) {
                save_.eeprom_write_bit(static_cast<uint16_t>(v));
                return;
            }
            log_unmapped(addr, v, true, 4);
            return;
        case Region::Io:
            if (is_memctl(off)) {
                memctl_write(static_cast<uint32_t>(off), v, 4);
                return;
            }
            io_dispatch_.write32(static_cast<uint32_t>(off), v);
            note_io_write(static_cast<uint32_t>(off), 4);
            return;
        case Region::Save:
            if (save_.sram_enabled()) {
                save_.sram_write(static_cast<uint32_t>(off),
                                 static_cast<uint8_t>(v & 0xFF));
                return;
            }
            if (save_.flash_enabled()) {
                save_.flash_write(static_cast<uint32_t>(off),
                                  static_cast<uint8_t>(v & 0xFF));
                return;
            }
            log_unmapped(addr, v, true, 4);
            return;
        case Region::OpenBus:
        case Region::Unknown:
            log_unmapped(addr, v, true, 4);
            return;
    }
}

// Per-region access cost: one base bus cycle plus the region's wait states
// for the access width and sequentiality, from the live wait-state table
// (WAITCNT for ROM/SRAM, internal memory control for EWRAM; fixed bus
// widths elsewhere). Sources: GBATEK "GBA Memory Map", "GBA System
// Control"; mGBA 1d201b22 memory.c. See gba_waitstates.h.
uint32_t GbaBus::access_cycles(uint32_t addr, uint8_t width,
                               bool sequential) const {
    return waits_access_cycles(*waits_, addr, width, sequential);
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
