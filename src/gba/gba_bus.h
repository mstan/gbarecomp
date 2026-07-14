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
#include "gba_rtc.h"
#include "gba_save.h"

namespace gbarecomp::debug { class SnapshotWriter; class SnapshotReader; }

namespace gba {

// Optional game-owned, read-only ROM literal override. This is intended for
// narrowly scoped, opt-in LLE patches where the original guest code must see a
// configuration-derived constant. Returning non-zero accepts *out_value.
extern "C" int (*g_rom_read32_override)(std::uint32_t address,
                                         std::uint32_t original_value,
                                         std::uint32_t* out_value);

// Write-chokepoint observer for the P6 sljit differential gate. While one is
// registered (gate validation only — null during all normal play and the gcc
// path), GbaBus::write{8,16,32} notify it BEFORE applying each store, so the
// gate can journal RAM writes (to roll a validation pass back) and detect/trap
// device writes. Region ids 0..4 are the five writable RAM regions (old/new
// meaningful, in the heal_gate GateRegion order EWRAM,IWRAM,PAL,VRAM,OAM);
// Device is everything else (IO/Save/RTC/ROM/unmapped — side-effectful, old
// undefined). on_bus_write returns true to SUPPRESS the store (the gate traps
// device writes during a shard's shadow validation pass so a buggy shard can't
// fire a stray DMA / IO poke).
enum class BusWriteRegion : int {
    Ewram = 0, Iwram = 1, Pal = 2, Vram = 3, Oam = 4, Device = 5
};
struct BusWriteObserver {
    virtual ~BusWriteObserver() = default;
    virtual bool on_bus_write(BusWriteRegion region, uint32_t off, uint32_t addr,
                              uint8_t width, uint32_t old_value,
                              uint32_t new_value) = 0;
    // Notified on a READ from a device (IO) register. The gate uses this to pin
    // functions that read IO: device registers (VCOUNT, timer counters, …)
    // advance with cycles during the interpreter pass, so a shadow-ticked shard
    // re-run would read different values — such functions can't be cleanly
    // shadow-validated. Default no-op (only the gate cares).
    virtual void on_bus_read(uint32_t /*addr*/) {}
};

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

    // ── Open-bus prefetch model (GBATEK "Reading from Unused/BIOS Memory") ──
    // The GBA has no pull resistors on the data bus: a read of the protected
    // BIOS region (00000000-00003FFF from outside the BIOS) or of any unmapped
    // address returns the most recently PRE-FETCHED opcode, not 0. Games rely on
    // it (MC-HP-002: a NULL-animation-group deref reads the BIOS region; the
    // open-bus value's high byte has bit7 set, so the frame-list walker
    // terminates — returning 0 made it march forever).
    //
    // latch_bios_prefetch() is called once per guest instruction WHILE PC is in
    // the BIOS, so when the BIOS hands control to the cart, bios_prefetch_ holds
    // the prefetch at the exit instruction (== mGBA's biosPrefetch; for the GBA
    // SWI return at BIOS 0x188 that is mem[0x190]=0xE3A02004). note_pc() stashes
    // the live PC each bus access so an UNMAPPED read returns the CURRENT
    // prefetch (the executing code's $+8/$+4 word).
    void latch_bios_prefetch(uint32_t pc, bool thumb) {
        bios_prefetch_ = prefetch_word(pc, thumb);
    }
    void note_pc(uint32_t pc, bool thumb) {
        open_bus_pc_ = pc;
        open_bus_thumb_ = thumb;
    }
    uint32_t bios_open_bus() const { return bios_prefetch_; }

    // Wire up the cartridge ROM bytes. The bus does NOT take
    // ownership; the caller must keep the buffer alive.
    void set_rom(const uint8_t* rom_bytes, std::size_t rom_size) {
        rom_ = rom_bytes;
        rom_size_ = rom_size;
        // Detect the cartridge RTC (Seiko S-3511A) by SDK signature and
        // arm the GPIO clock if present. No-op for non-clock carts.
        rtc_.configure(rom_bytes, rom_size);
        // Arm the MP2K audio shadow mixer (QoL; off unless the ROM links
        // MP2K and it's requested via [audio].shadow / GBARECOMP_AUDIO_SHADOW).
        // The region pointers back its side-effect-free MemView.
        audio_.configure_shadow(mp2k_detect(rom_bytes, rom_size),
                                rom_bytes, rom_size,
                                ewram_.data(), ewram_.size(),
                                iwram_.data(), iwram_.size(),
                                audio_shadow_request_);
    }

    // Per-game default for the MP2K audio shadow ([audio].shadow). Set by
    // the runtime before set_rom; GBARECOMP_AUDIO_SHADOW still overrides.
    void request_audio_shadow(bool on) { audio_shadow_request_ = on; }

    // Access to the IO dispatcher. Callers wire PPU/IRQ pointers in
    // before running code.
    GbaIo&       io()       { return io_dispatch_; }
    const GbaIo& io() const { return io_dispatch_; }

    GbaAudio&       audio()       { return audio_; }
    const GbaAudio& audio() const { return audio_; }

    GbaSave&       save()       { return save_; }
    const GbaSave& save() const { return save_; }

    GbaRtc&       rtc()       { return rtc_; }
    const GbaRtc& rtc() const { return rtc_; }

    // Region accessors — useful for tests, debug snapshots, and the
    // PPU (which reads VRAM/OAM/PAL directly to render).
    uint8_t* ewram_ptr() { return ewram_.data(); }
    uint8_t* iwram_ptr() { return iwram_.data(); }
    uint8_t* pal_ptr()   { return pal_.data();   }
    uint8_t* vram_ptr()  { return vram_.data();  }
    uint8_t* oam_ptr()   { return oam_.data();   }

    // Cartridge ROM bytes (the buffer set via set_rom; not owned).
    // Used by observability tooling that scans the image, e.g. MP2K
    // driver detection (src/gba/gba_m4a).
    const uint8_t* rom_ptr() const { return rom_; }
    std::size_t    rom_size() const { return rom_size_; }

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

    // Register (or clear, with nullptr) the P6 gate write observer. Null in all
    // normal play / the gcc path → the hook is one predicted-untaken branch per
    // store. Owned by the caller (the gate); GbaBus only holds the pointer.
    void set_write_observer(BusWriteObserver* obs) { write_observer_ = obs; }
    BusWriteObserver* exchange_write_observer(BusWriteObserver* obs) {
        BusWriteObserver* previous = write_observer_;
        write_observer_ = obs;
        return previous;
    }

private:
    // Notify a registered write observer of a pending store; returns true if the
    // store should be SUPPRESSED (device trap during shadow validation). Reads
    // the pre-store value for RAM regions so the gate can journal/roll back.
    bool observe_write(Region region, std::size_t off, uint32_t addr,
                       uint8_t width, uint32_t new_value);
    // The opcode word the CPU would have prefetched at `pc` ($+8 in ARM, the
    // $+4 halfword mirrored into both halves in THUMB), read from the BIOS or
    // ROM image. Drives the open-bus model above. 0 when the prefetch source
    // is itself unmapped.
    uint32_t prefetch_word(uint32_t pc, bool thumb) const;

    const GbaBios* bios_ = nullptr;
    const uint8_t* rom_  = nullptr;
    std::size_t    rom_size_ = 0;
    bool           bios_access_enabled_ = true;
    bool           audio_shadow_request_ = false;
    uint32_t       bios_prefetch_ = 0;   // latched BIOS open-bus value
    uint32_t       open_bus_pc_   = 0;   // live PC for unmapped open-bus
    bool           open_bus_thumb_ = false;

    std::array<uint8_t, 256 * 1024> ewram_{};
    std::array<uint8_t,  32 * 1024> iwram_{};
    std::array<uint8_t,        1024> pal_{};
    std::array<uint8_t,  96 * 1024> vram_{};
    std::array<uint8_t,        1024> oam_{};

    GbaIo       io_dispatch_;
    GbaAudio    audio_;
    GbaSave     save_;
    GbaRtc      rtc_;

    uint32_t    last_fetched_   = 0;
    std::size_t unmapped_count_ = 0;

    BusWriteObserver* write_observer_ = nullptr;  // P6 gate hook; null in play
};

}  // namespace gba
