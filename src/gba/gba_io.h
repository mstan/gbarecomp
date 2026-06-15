// gba_io.h — GBA IO register dispatcher (0x04000000..0x040003FF).
//
// Design:
//   - 1 KB byte-backed storage so most registers behave like a flat
//     RAM region. Many GBA registers are simple R/W bit fields with no
//     side effect on access; backing them with bytes "just works."
//   - Special-cased registers (with read/write side effects) intercept
//     the access first and then update / consult the backing array as
//     needed:
//       * VCOUNT — read-only; reflects PPU state.
//       * DISPSTAT — read-only flag bits 0..2 (VBlank/HBlank/VCount)
//         must be preserved through writes; bits 3..15 are writable.
//       * IF — write-1-to-clear (writing 1 acknowledges the IRQ).
//       * IME — single-bit master interrupt enable.
//       * POSTFLG — boot flag; 0 on cold boot.
//       * HALTCNT — write-only; values: 0x00 = HALT, 0x80 = STOP.
//   - Per PRINCIPLES.md "Unknown IO never silently returns a magic
//     value": addresses outside the documented IO range, or to
//     reserved bytes inside it, still emit `log_unmapped`.
//
// References:
//   - GBATEK § "GBA I/O Map"
//   - GBATEK § "GBA Display Status" (DISPSTAT), § "GBA Interrupt
//     Control" (IE/IF/IME), § "GBA Halt / Stop / Sleep"

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "bus.h"   // armv4t::Bus — used for DMA byte transfers

namespace gbarecomp::debug { class SnapshotWriter; class SnapshotReader; }

namespace gba {

class GbaPpu;
class GbaIrq;
class GbaAudio;

// Documented IO register addresses (all relative to 0x04000000).
namespace IoReg {
constexpr uint32_t DISPCNT   = 0x000;  // u16
constexpr uint32_t DISPSTAT  = 0x004;  // u16
constexpr uint32_t VCOUNT    = 0x006;  // u16 (read-only)
constexpr uint32_t BG0CNT    = 0x008;
constexpr uint32_t BG1CNT    = 0x00A;
constexpr uint32_t BG2CNT    = 0x00C;
constexpr uint32_t BG3CNT    = 0x00E;
constexpr uint32_t SOUNDBIAS = 0x088;  // u16
constexpr uint32_t IE        = 0x200;  // u16
constexpr uint32_t IF        = 0x202;  // u16  (write-1-to-clear)
constexpr uint32_t WAITCNT   = 0x204;  // u16
constexpr uint32_t IME       = 0x208;  // u16/u32
constexpr uint32_t POSTFLG   = 0x300;  // u8
constexpr uint32_t HALTCNT   = 0x301;  // u8   (write-only)
constexpr uint32_t KEYINPUT  = 0x130;  // u16 (read-only)
constexpr uint32_t KEYCNT    = 0x132;  // u16
}  // namespace IoReg

class GbaIo {
public:
    static constexpr std::size_t kIoSize = 0x400;

    GbaIo();
    ~GbaIo();

    // Optional wiring. PPU is consulted for VCOUNT/DISPSTAT flag bits;
    // IRQ controller is the source of truth for IE/IF/IME (but for
    // Phase 2.2 we just back them in the IO array directly until the
    // real IRQ scheduler arrives).
    void set_ppu(GbaPpu* p) { ppu_ = p; }
    void set_irq(GbaIrq* r) { irq_ = r; }
    // The bus is needed to execute DMA transfers; without it, writes
    // to DMAxCNT_H silently store the register without copying any
    // bytes and the BIOS's VRAM/PAL/OAM uploads vanish.
    void set_bus(armv4t::Bus* b) { bus_ = b; }
    void set_audio(GbaAudio* a) { audio_ = a; }

    // Bus-side entry points. `off` is the offset into the IO region
    // (0x00..0x3FF). Out-of-range offsets are filtered by the bus.
    uint8_t  read8 (uint32_t off);
    uint16_t read16(uint32_t off);
    uint32_t read32(uint32_t off);
    void     write8 (uint32_t off, uint8_t  v);
    void     write16(uint32_t off, uint16_t v);
    void     write32(uint32_t off, uint32_t v);

    // Whether the CPU should HALT until the next IRQ. Set when the
    // BIOS writes 0x00 to HALTCNT. The fetch loop checks this and
    // skips instruction execution until a pending IRQ clears it.
    bool halted() const { return halted_; }
    void clear_halt() { halted_ = false; }

    // IRQ controller surface. Bit positions per GBATEK § "GBA
    // Interrupt Control" — VBlank=0, HBlank=1, VCount=2, Timer0..3 =
    // 3..6, Serial=7, DMA0..3 = 8..11, Keypad=12, Cartridge=13.
    enum IrqBit : uint16_t {
        IrqVBlank = 1u << 0,
        IrqHBlank = 1u << 1,
        IrqVCount = 1u << 2,
        IrqTimer0 = 1u << 3,
        IrqTimer1 = 1u << 4,
        IrqTimer2 = 1u << 5,
        IrqTimer3 = 1u << 6,
        IrqSerial = 1u << 7,
        IrqDma0   = 1u << 8,
        IrqDma1   = 1u << 9,
        IrqDma2   = 1u << 10,
        IrqDma3   = 1u << 11,
        IrqKeypad = 1u << 12,
        IrqCart   = 1u << 13,
    };

    // Set the bit in IF — devices call this when their event fires.
    // Whether the CPU actually takes the IRQ depends on IE / IME /
    // CPSR.I, checked separately.
    void request_irq(uint16_t bit);

    // True if (IE & IF) != 0 AND IME is set. The CPU's CPSR.I check
    // happens at the call site.
    bool irq_pending() const;

    // Raw register reads for the runtime's CPU IRQ entry path.
    uint16_t ie()      const;
    uint16_t if_reg()  const;
    bool     ime()     const;
    uint16_t dispstat() const;

    // For tests / debug.
    std::size_t unmapped_count() const { return unmapped_count_; }
    void reset_unmapped_count() { unmapped_count_ = 0; }
    const uint8_t* raw() const { return io_.data(); }
    std::size_t dma_runs(int ch) const { return dma_runs_[ch]; }
    std::size_t dma_words(int ch) const { return dma_words_[ch]; }

    // Trigger an immediate-mode DMA on the given channel (0..3).
    // Called internally when CNT_H is written with enable+mode=0.
    void run_immediate_dma(int channel);

    // Run all channels armed for a timed start mode (1=VBlank, 2=HBlank), one
    // transfer block each, advancing the running SAD/DAD across triggers.
    // Called per VBlank-start / per visible-line HBlank-start by the device
    // tick. Repeat (CNT_H bit9) keeps the channel armed (reloading DAD when
    // dest_ctrl==3); otherwise the enable bit is cleared. This delivers the
    // per-scanline WIN0H table that draws a transition's circular iris
    // (MC-HP-003) — without it WIN0H gets one value and the iris is a rectangle.
    void run_timed_dma(int start_mode);

    // Host-side input update. The low 10 bits are GBA KEYINPUT
    // (active-low: 1 = released, 0 = pressed). Stored directly into
    // the IO backing so the next CPU read sees the current state.
    void set_keyinput(uint16_t keys);

    // Advance hardware timers by CPU cycles. Timer overflows raise IRQs
    // and clock direct-sound FIFOs.
    void tick_timers(uint32_t cycles);
    uint32_t cycles_until_next_timer_event() const;

    // Save-state serialization. Captures the 1 KB IO page, halt flag,
    // and timer/DMA shadow state. Live wiring (ppu_/irq_/bus_/audio_)
    // is NOT serialized — it stays connected across a restore. See
    // debug/snapshot.h.
    void serialize(gbarecomp::debug::SnapshotWriter& w) const;
    void deserialize(gbarecomp::debug::SnapshotReader& r);

private:
    GbaPpu*       ppu_   = nullptr;
    GbaIrq*       irq_   = nullptr;
    armv4t::Bus*  bus_   = nullptr;
    GbaAudio*     audio_ = nullptr;

    // Flat backing for the 1 KB IO region. Anything not specially
    // handled is just read/written here.
    std::array<uint8_t, kIoSize> io_{};

    bool halted_ = false;
    std::size_t unmapped_count_ = 0;
    std::size_t dma_runs_[4]  = {0, 0, 0, 0};
    std::size_t dma_words_[4] = {0, 0, 0, 0};
    uint16_t timer_reload_[4] = {0, 0, 0, 0};
    uint16_t timer_counter_[4] = {0, 0, 0, 0};
    uint16_t timer_control_[4] = {0, 0, 0, 0};
    uint32_t timer_accum_[4] = {0, 0, 0, 0};
    uint32_t dma_next_source_[4] = {0, 0, 0, 0};
    uint32_t dma_next_dest_[4] = {0, 0, 0, 0};

    // Log an unknown / unhandled IO access. Logs once per (offset,
    // direction, width) tuple to keep the output bounded under the
    // BIOS's setup phase (which touches hundreds of registers).
    void warn_unhandled(uint32_t off, uint32_t value, bool is_write, uint8_t width);
    void run_sound_fifo_dma(int channel);
};

}  // namespace gba
