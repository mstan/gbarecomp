// gba_bus.h — Concrete MemoryBus for the GBA.
//
// Owns backing storage for EWRAM (256 KB), IWRAM (32 KB), PAL (1 KB),
// VRAM (96 KB), OAM (1 KB). BIOS reads delegate to the GbaBios
// instance the runtime owns; ROM reads delegate to a byte buffer set
// by the runtime after loading the cartridge.
//
// IO accesses dispatch to an owned GbaIo (1 KB IO page handler).
// Unknown IO offsets still surface via log_unmapped(), but recognised
// registers persist across reads/writes — without that, the BIOS's
// SOUNDBIAS-ramp loop spins forever waiting for hardware state.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "gba_bios.h"
#include "gba_io.h"
#include "gba_memory.h"

namespace gba {

class GbaBus : public MemoryBus {
public:
    GbaBus();
    ~GbaBus() override;

    // Wire up the BIOS image. Reads from 0x00000000..0x00003FFF go
    // through `bios->read*()`. Pass nullptr to leave BIOS region
    // unmapped (reads return open-bus; writes are silently dropped).
    void set_bios(const GbaBios* bios) { bios_ = bios; }

    // Wire up the cartridge ROM bytes. The bus does NOT take
    // ownership; the caller must keep the buffer alive.
    void set_rom(const uint8_t* rom_bytes, std::size_t rom_size) {
        rom_ = rom_bytes;
        rom_size_ = rom_size;
    }

    // Access to the IO dispatcher. Callers wire PPU/IRQ pointers in
    // before running code.
    GbaIo&       io()       { return io_dispatch_; }
    const GbaIo& io() const { return io_dispatch_; }

    // Region accessors — useful for tests, debug snapshots, and the
    // PPU (which reads VRAM/OAM/PAL directly to render).
    uint8_t* ewram_ptr() { return ewram_.data(); }
    uint8_t* iwram_ptr() { return iwram_.data(); }
    uint8_t* pal_ptr()   { return pal_.data();   }
    uint8_t* vram_ptr()  { return vram_.data();  }
    uint8_t* oam_ptr()   { return oam_.data();   }

    const uint8_t* ewram_ptr() const { return ewram_.data(); }
    const uint8_t* iwram_ptr() const { return iwram_.data(); }
    const uint8_t* pal_ptr()   const { return pal_.data();   }
    const uint8_t* vram_ptr()  const { return vram_.data();  }
    const uint8_t* oam_ptr()   const { return oam_.data();   }

    uint8_t  read8 (uint32_t addr) override;
    uint16_t read16(uint32_t addr) override;
    uint32_t read32(uint32_t addr) override;

    void write8 (uint32_t addr, uint8_t  v) override;
    void write16(uint32_t addr, uint16_t v) override;
    void write32(uint32_t addr, uint32_t v) override;

    void log_unmapped(uint32_t addr,
                      uint32_t value,
                      bool is_write,
                      uint8_t  width) override;

    // Debug / test introspection.
    std::size_t unmapped_count() const { return unmapped_count_; }
    void reset_unmapped_count() { unmapped_count_ = 0; }

    // The last-fetched instruction word. ARMv4T open-bus reads from
    // unmapped addresses return this value (or a derived value); we
    // expose it so the fetch loop can keep it current.
    void set_last_fetched(uint32_t word) { last_fetched_ = word; }
    uint32_t last_fetched() const { return last_fetched_; }

private:
    const GbaBios* bios_ = nullptr;
    const uint8_t* rom_  = nullptr;
    std::size_t    rom_size_ = 0;

    std::array<uint8_t, 256 * 1024> ewram_{};
    std::array<uint8_t,  32 * 1024> iwram_{};
    std::array<uint8_t,        1024> pal_{};
    std::array<uint8_t,  96 * 1024> vram_{};
    std::array<uint8_t,        1024> oam_{};

    GbaIo       io_dispatch_;

    uint32_t    last_fetched_   = 0;
    std::size_t unmapped_count_ = 0;
};

}  // namespace gba
