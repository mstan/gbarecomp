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

#include "gba_audio.h"
#include "gba_bios.h"
#include "gba_io.h"
#include "gba_memory.h"
#include "gba_save.h"

namespace gbarecomp::debug { class SnapshotWriter; class SnapshotReader; }

namespace gba {

class GbaBus : public MemoryBus {
public:
    GbaBus();
    ~GbaBus() override;

    // Wire up the BIOS image. Reads from 0x00000000..0x00003FFF go
    // through `bios->read*()` only while BIOS access is enabled. Pass
    // nullptr to leave BIOS region unmapped.
    void set_bios(const GbaBios* bios) { bios_ = bios; }

    // Real GBA BIOS bytes are protected after the BIOS hands control
    // to cartridge code. Runtime-generated reads update this from the
    // executing PC before each bus access; standalone BIOS/interpreter
    // tooling leaves the default enabled state intact.
    void set_bios_access_enabled(bool enabled) {
        bios_access_enabled_ = enabled;
    }
    bool bios_access_enabled() const { return bios_access_enabled_; }

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

    GbaAudio&       audio()       { return audio_; }
    const GbaAudio& audio() const { return audio_; }

    GbaSave&       save()       { return save_; }
    const GbaSave& save() const { return save_; }

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

    // Per-region S/N-cycle table per GBATEK § "GBA Memory Map" and
    // "GBA Cycle Times". Drives interpreter cycle accounting so our
    // PPU/animation timing matches mGBA's after every CPU step.
    uint32_t access_cycles(uint32_t addr, uint8_t width,
                           bool sequential) const override;

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

    // Save-state serialization of the bus-owned memory regions
    // (EWRAM/IWRAM/PAL/VRAM/OAM) plus bus-local scalars. The owned
    // GbaIo / GbaAudio / GbaSave are serialized separately by the
    // orchestrator via io()/audio()/save(). The BIOS image and ROM
    // bytes are NOT serialized — they are reloaded (and hash-verified)
    // on launch, and the snapshot's ROM-SHA-1 gate guarantees a match.
    // See debug/snapshot.h.
    void serialize(gbarecomp::debug::SnapshotWriter& w) const;
    void deserialize(gbarecomp::debug::SnapshotReader& r);

private:
    const GbaBios* bios_ = nullptr;
    const uint8_t* rom_  = nullptr;
    std::size_t    rom_size_ = 0;
    bool           bios_access_enabled_ = true;

    std::array<uint8_t, 256 * 1024> ewram_{};
    std::array<uint8_t,  32 * 1024> iwram_{};
    std::array<uint8_t,        1024> pal_{};
    std::array<uint8_t,  96 * 1024> vram_{};
    std::array<uint8_t,        1024> oam_{};

    GbaIo       io_dispatch_;
    GbaAudio    audio_;
    GbaSave     save_;

    uint32_t    last_fetched_   = 0;
    std::size_t unmapped_count_ = 0;
};

}  // namespace gba
