// gba_ppu.h — GBA PPU minimal state.
//
// Phase 2.3 scope: enough PPU to advance VCOUNT and the DISPSTAT
// VBlank/HBlank/VCount-match flags. Real rendering (BG modes,
// sprites, blending, etc.) lands in Phase 2.4+.
//
// Cycle accounting per GBATEK § "GBA Picture Processing Unit":
//   1 dot           = 4 cycles
//   visible window  = 240 dots (960 cycles) per scanline
//   HBlank window   = 68 dots  (272 cycles) per scanline
//   scanline total  = 308 dots / 1232 cycles
//   visible lines   = 160
//   VBlank lines    = 68 (scanlines 160..227)
//   frame total     = 228 scanlines = 280896 cycles
//
// VCOUNT increments at the start of each scanline. HBlank flag sets
// at dot 240 (cycle 960 of the line), clears at start of next line.

#pragma once

#include <cstdint>
#include <cstddef>

namespace gba {

class GbaPpu {
public:
    // Hardware constants.
    static constexpr uint32_t kDotsVisible      = 240;
    static constexpr uint32_t kDotsHBlank       = 68;
    static constexpr uint32_t kDotsPerScanline  = kDotsVisible + kDotsHBlank;  // 308
    static constexpr uint32_t kCyclesPerDot     = 4;
    static constexpr uint32_t kCyclesPerScanline =
        kDotsPerScanline * kCyclesPerDot;                                       // 1232
    static constexpr uint32_t kLinesVisible     = 160;
    static constexpr uint32_t kLinesTotal       = 228;
    static constexpr uint32_t kCyclesPerFrame   =
        kLinesTotal * kCyclesPerScanline;                                       // 280896

    GbaPpu();
    ~GbaPpu();

    // Events that fired during a tick. The caller routes these to
    // the IRQ controller (gating on the DISPSTAT enable bits the BIOS
    // sets). Keeping events as a return value rather than a callback
    // keeps the PPU pure / portable.
    struct TickEvents {
        bool vblank_started = false;  // scanline crossed from 159 → 160
        bool hblank_started = false;  // dot crossed visible → HBlank
        bool vcount_matched = false;  // scanline became == compare value
        bool frame_completed = false; // scanline wrapped 227 → 0
    };

    // Advance the PPU by `cycles` host cycles. Increments dot/scanline
    // counters and toggles VBlank/HBlank flags appropriately. The
    // caller (scheduler) accumulates the cycle budget per CPU step.
    //
    // `vcount_compare` is the value DISPSTAT[15:8] specifies; we
    // surface VCount-match in the returned events.
    TickEvents tick(uint32_t cycles, uint16_t vcount_compare = 0xFFFFu);

    // Live state for IO reads.
    uint16_t vcount() const { return static_cast<uint16_t>(vcount_); }
    bool     in_vblank() const { return scanline_ >= kLinesVisible; }
    bool     in_hblank() const { return dot_in_scanline_ >= kDotsVisible; }

    // VCount-match: returns true when current VCOUNT equals the value
    // configured in DISPSTAT bits 8..15 (the IO layer holds that
    // value; we just expose vcount() for the comparison).
    // For Phase 2.3 the bus owns the comparison; this stays simple.

    // Reset to scanline 0, dot 0. Used by the runtime on cold boot.
    void reset();

    // Total frames completed since reset — useful for sync points.
    uint64_t frame_count() const { return frame_count_; }

    // Render the current frame into a 240*160 RGB888 buffer.
    // Phase 2.4 scope: OBJ (sprite) rendering for non-affine
    // sprites only. BG layers and affine sprites land in 2.5+.
    static constexpr uint32_t kScreenWidth  = 240;
    static constexpr uint32_t kScreenHeight = 160;
    static constexpr std::size_t kFramebufferBytes =
        kScreenWidth * kScreenHeight * 3;

    void render(uint8_t* rgb,
                uint16_t dispcnt,
                const uint8_t* io,    // 1 KB IO page for BGxCNT, BGxX/Y/PA/PB/PC/PD
                const uint8_t* vram,
                const uint8_t* oam,
                const uint8_t* pal) const;

private:
    uint32_t scanline_        = 0;   // 0..227
    uint32_t dot_in_scanline_ = 0;   // 0..307 (in dots, not cycles)
    uint32_t cycle_in_dot_    = 0;   // 0..3
    uint16_t vcount_          = 0;
    uint64_t frame_count_     = 0;
};

}  // namespace gba
