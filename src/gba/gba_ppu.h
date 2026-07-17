// gba_ppu.h — GBA PPU minimal state.
//
// Phase 2.3 scope: enough PPU to advance VCOUNT and the DISPSTAT
// VBlank/HBlank/VCount-match flags. Real rendering (BG modes,
// sprites, blending, etc.) lands in Phase 2.4+.
//
// Cycle accounting per GBATEK § "GBA Picture Processing Unit":
//   1 dot           = 4 cycles
//   HDraw window    = 252 dots (1008 cycles) per scanline
//   HBlank window   = 56 dots  (224 cycles) per scanline
//   scanline total  = 308 dots / 1232 cycles
//   visible lines   = 160
//   VBlank lines    = 68 (scanlines 160..227)
//   frame total     = 228 scanlines = 280896 cycles
//
// VCOUNT increments at the start of each scanline. HBlank flag sets
// at dot 252 (cycle 1008 of the line), clears at start of next line.

#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace gbarecomp::debug { class SnapshotWriter; class SnapshotReader; }

namespace gba {

class GbaPpu {
public:
    // Hardware constants.
    static constexpr uint32_t kDotsVisible      = 252;
    static constexpr uint32_t kDotsHBlank       = 56;
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
    uint32_t cycles_until_next_event() const;

    // VCount-match: returns true when current VCOUNT equals the value
    // configured in DISPSTAT bits 8..15 (the IO layer holds that
    // value; we just expose vcount() for the comparison).
    // For Phase 2.3 the bus owns the comparison; this stays simple.

    // Reset to scanline 0, dot 0. Used by the runtime on cold boot.
    void reset();

    // Save-state serialization (all internal timing + latched frame).
    // No live pointers to skip. See debug/snapshot.h.
    void serialize(gbarecomp::debug::SnapshotWriter& w) const;
    void deserialize(gbarecomp::debug::SnapshotReader& r);

    // Total frames completed since reset — useful for sync points.
    uint64_t frame_count() const { return frame_count_; }

    // Render the current frame into a 240*160 RGB888 buffer.
    // Phase 2.4 scope: OBJ (sprite) rendering for non-affine
    // sprites only. BG layers and affine sprites land in 2.5+.
    static constexpr uint32_t kScreenWidth  = 240;
    static constexpr uint32_t kScreenHeight = 160;
    static constexpr std::size_t kFramebufferBytes =
        kScreenWidth * kScreenHeight * 3;

    // ── View-area expansion (opt-in enhancement; default OFF = faithful) ──
    // Generic "render extra margin columns/rows" runner capability. With every
    // margin 0 the PPU runs the LITERAL vanilla path (render_scanline_internal),
    // so OFF-mode is byte-identical to the faithful build BY CONSTRUCTION — not
    // by algebraic equivalence (per the widescreen design review). Horizontal
    // widening reuses each scanline's latched register state; VERTICAL widening
    // would have to invent state for nonexistent scanlines (no VCOUNT / HBlank
    // DMA / affine-ref / WIN_V), so kMaxExtraY is compile-capped at 0 for now —
    // the API still accepts top/bottom (clamped to 0) so callers stay generic.
    // These margins are present-time host state and are NEVER serialized into
    // the snapshot (the save format is unchanged).
    // Experimental horizontal envelope: 120 pixels per side gives an exact
    // 480x160 maximum while the default remains the literal 240x160 path.
    static constexpr uint32_t kMaxExtraX = 120;
    static constexpr uint32_t kMaxExtraY = 0;    // vertical deferred; bump when invented-scanline path lands
    static constexpr uint32_t kMaxRenderWidth  = kScreenWidth  + 2u * kMaxExtraX;  // 480
    static constexpr uint32_t kMaxRenderHeight = kScreenHeight + 2u * kMaxExtraY;  // 160
    static constexpr std::size_t kMaxFramebufferBytes =
        static_cast<std::size_t>(kMaxRenderWidth) * kMaxRenderHeight * 3;

    // Set the per-side view margins (clamped to the compile-time max per axis).
    // Call once at runtime init from the runner's --widescreen wiring.
    void set_view_margins(uint32_t left, uint32_t right,
                          uint32_t top, uint32_t bottom);
    uint32_t view_extra_left()   const { return extra_left_; }
    uint32_t view_extra_right()  const { return extra_right_; }
    uint32_t view_extra_top()    const { return extra_top_; }
    uint32_t view_extra_bottom() const { return extra_bottom_; }
    bool     view_expanded()     const {
        return (extra_left_ | extra_right_ | extra_top_ | extra_bottom_) != 0;
    }
    // Active output dimensions = vanilla + active margins (== 240/160 when OFF).
    uint32_t render_width()  const { return kScreenWidth  + extra_left_ + extra_right_; }
    uint32_t render_height() const { return kScreenHeight + extra_top_ + extra_bottom_; }
    std::size_t render_bytes() const {
        return static_cast<std::size_t>(render_width()) * render_height() * 3;
    }

    void render(uint8_t* rgb,
                uint16_t dispcnt,
                const uint8_t* io,    // 1 KB IO page for BGxCNT, BGxX/Y/PA/PB/PC/PD
                const uint8_t* vram,
                const uint8_t* oam,
                const uint8_t* pal) const;
    void render_scanline(uint32_t y,
                         uint16_t dispcnt,
                         const uint8_t* io,
                         const uint8_t* vram,
                         const uint8_t* oam,
                         const uint8_t* pal);

    // Latch the just-finished visible frame. The BIOS mutates OAM/PAL
    // during VBlank; screenshots must therefore use the frame captured
    // at VBlank start, not whatever live memory contains later.
    void latch_framebuffer(uint16_t dispcnt,
                           const uint8_t* io,
                           const uint8_t* vram,
                           const uint8_t* oam,
                           const uint8_t* pal);
    void mark_framebuffer_latched();
    const uint8_t* latched_framebuffer() const { return latched_fb_.data(); }
    bool has_latched_framebuffer() const { return has_latched_fb_; }

private:
    uint32_t scanline_        = 0;   // 0..227
    uint32_t dot_in_scanline_ = 0;   // 0..307 (in dots, not cycles)
    uint32_t cycle_in_dot_    = 0;   // 0..3
    uint16_t vcount_          = 0;
    uint64_t frame_count_     = 0;
    // Oversized to the compile-time max so the widened frame fits without
    // reallocation; only the first render_bytes() are used (vanilla = 115200).
    std::array<uint8_t, kMaxFramebufferBytes> latched_fb_{};
    bool has_latched_fb_ = false;

    // View-area margins (present-time host state; NOT serialized — see above).
    uint32_t extra_left_   = 0;
    uint32_t extra_right_  = 0;
    uint32_t extra_top_    = 0;
    uint32_t extra_bottom_ = 0;
};

// Widescreen margin tilemap provider (Step C; see gba_ppu.cpp). Set by the
// runtime-side sidecar; nullptr = vanilla wide behavior.
extern "C" int (*g_ws_tilemap_provider)(int bg, int hw_x, int screen_y,
                                        uint16_t* out_entry);
// Optional per-game presentation remap for regular BG samples in the expanded
// renderer. The callback receives the physical output X and may suppress the
// sample (-1), leave the hardware X unchanged (0), or provide an authentic
// hardware X through out_hw_x (1). The native 240x160 renderer never calls it.
extern "C" int (*g_ws_bg_x_provider)(int bg, int output_x, int screen_y,
                                     int* out_hw_x);
// Per-game policy for self-sufficient authored margin providers. Off keeps the
// established fail-closed window/savestate behavior. On lets provider-sourced
// regular BG margins continue independently beside native HUD/dialog windows.
extern "C" int g_ws_authored_margin_layers;
extern "C" int g_ws_pillarbox;  // Step C policy: black margins on non-field screens
extern "C" int g_ws_pillarbox_left;
extern "C" int g_ws_pillarbox_right;
// Optional per-game interpretation of the 9-bit OBJ X field in expanded-view
// rendering. Hardware-faithful signed decoding remains the default.
extern "C" int (*g_ws_obj_x_provider)(int raw_x, int* out_x);
// Richer per-game OBJ placement hook for screen-space UI. Called only by the
// expanded renderer and before g_ws_obj_x_provider; receives the OAM index and
// raw attributes so a game can distinguish HUD sprites from world objects.
extern "C" int (*g_ws_obj_attr_x_provider)(int oam_index,
                                            std::uint16_t attr0,
                                            std::uint16_t attr1,
                                            std::uint16_t attr2,
                                            int* out_x);

}  // namespace gba
