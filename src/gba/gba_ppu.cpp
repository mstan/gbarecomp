// gba_ppu.cpp — see gba_ppu.h.

#include "gba_ppu.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gba {

GbaPpu::GbaPpu()  = default;
GbaPpu::~GbaPpu() = default;

void GbaPpu::reset() {
    scanline_ = 0;
    dot_in_scanline_ = 0;
    cycle_in_dot_ = 0;
    vcount_ = 0;
    frame_count_ = 0;
}

GbaPpu::TickEvents GbaPpu::tick(uint32_t cycles, uint16_t vcount_compare) {
    // Coarse but correct: walk dot-by-dot, advancing scanline counters
    // as we cross dot boundaries. The cycle_in_dot_ accumulator lets
    // sub-dot ticks (less than 4 cycles) stash residue for the next
    // call.
    TickEvents ev{};
    cycle_in_dot_ += cycles;
    while (cycle_in_dot_ >= kCyclesPerDot) {
        cycle_in_dot_ -= kCyclesPerDot;
        bool was_visible = (dot_in_scanline_ < kDotsVisible);
        ++dot_in_scanline_;
        if (was_visible && dot_in_scanline_ == kDotsVisible) {
            ev.hblank_started = true;
        }
        if (dot_in_scanline_ >= kDotsPerScanline) {
            dot_in_scanline_ = 0;
            bool was_in_visible_window = (scanline_ < kLinesVisible);
            ++scanline_;
            if (scanline_ >= kLinesTotal) {
                scanline_ = 0;
                ++frame_count_;
                ev.frame_completed = true;
            }
            vcount_ = static_cast<uint16_t>(scanline_);
            if (was_in_visible_window && scanline_ == kLinesVisible) {
                ev.vblank_started = true;
            }
            if (vcount_ == vcount_compare) {
                ev.vcount_matched = true;
            }
        }
    }
    return ev;
}

// ─────────────────────────────────────────────────────────────────────
// Renderer (Phase 2.4)
// ─────────────────────────────────────────────────────────────────────
//
// Scope: enough to draw the GBA BIOS boot intro. The BIOS intro uses
// DISPCNT = 0x1002 (BG mode 2, OBJ enabled, all BGs disabled), so we
// only need OBJ (sprite) compositing for this milestone.
//
// References: GBATEK § "GBA OBJs - OAM", § "GBA Palettes",
//             § "GBA VRAM Character Data".

namespace {

// Sprite shape × size → (width, height) in pixels.
//   shape: 0=square, 1=horizontal, 2=vertical
//   size:  0..3
constexpr int kSpriteWH[3][4][2] = {
    // square
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    // horizontal
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    // vertical
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

// Convert 16-bit GBA color (0BBBBBGGGGGRRRRR) to 24-bit RGB888.
inline void to_rgb888(uint16_t c, uint8_t* out) {
    // 5 → 8 bit expansion: ((v << 3) | (v >> 2)) gives full-range 0..255.
    uint8_t r = (c >>  0) & 0x1F;
    uint8_t g = (c >>  5) & 0x1F;
    uint8_t b = (c >> 10) & 0x1F;
    out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
    out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
}

inline uint16_t load_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

// Read a signed 16-bit Q8.8 affine parameter from IO.
static inline int32_t read_s16(const uint8_t* io, uint32_t off) {
    int16_t v = static_cast<int16_t>(
        io[off] | (io[off + 1] << 8));
    return static_cast<int32_t>(v);
}

// Read a signed 28-bit Q19.8 reference coord from IO (BGxX/BGxY).
// Bits 27..0 are the value; the field is sign-extended from bit 27.
static inline int32_t read_s28_ref(const uint8_t* io, uint32_t off) {
    uint32_t v = static_cast<uint32_t>(io[off]) |
                 (static_cast<uint32_t>(io[off + 1]) <<  8) |
                 (static_cast<uint32_t>(io[off + 2]) << 16) |
                 (static_cast<uint32_t>(io[off + 3]) << 24);
    v &= 0x0FFFFFFFu;
    if (v & 0x08000000u) v |= 0xF0000000u;  // sign-extend bit 27
    return static_cast<int32_t>(v);
}

// Render one affine BG (BG2 or BG3) into the framebuffer. Pixels at
// palette index 0 are skipped (transparent). `bgcnt_off` is the IO
// offset of BGxCNT (0x0C for BG3, 0x0A for BG2). `bg_params_off` is
// the offset of BGxPA (0x20 for BG2, 0x30 for BG3).
namespace {

void render_affine_bg(uint8_t* rgb,
                      const uint8_t* io,
                      const uint8_t* vram,
                      const uint8_t* pal,
                      uint32_t bgcnt_off,
                      uint32_t bg_params_off,
                      uint32_t kScreenWidth,
                      uint32_t kScreenHeight) {
    uint16_t bgcnt = static_cast<uint16_t>(
        io[bgcnt_off] | (io[bgcnt_off + 1] << 8));
    uint32_t char_base   = ((bgcnt >> 2) & 0x3u) * 0x4000u;  // 16 KB blocks
    uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;  // 2 KB blocks
    bool wrap            = (bgcnt & 0x2000u) != 0;
    uint32_t size_code   = (bgcnt >> 14) & 0x3u;
    int bg_pixels = 128 << size_code;   // 128, 256, 512, 1024
    int bg_tiles  = bg_pixels / 8;

    // Affine matrix and reference (PA at bg_params_off, then PB, PC,
    // PD, X, Y).
    int32_t pa = read_s16(io, bg_params_off + 0);
    int32_t pb = read_s16(io, bg_params_off + 2);
    int32_t pc = read_s16(io, bg_params_off + 4);
    int32_t pd = read_s16(io, bg_params_off + 6);
    int32_t refx = read_s28_ref(io, bg_params_off + 8);
    int32_t refy = read_s28_ref(io, bg_params_off + 12);

    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        // Per-scanline affine: (rx, ry) = ref + (0,y) projected.
        // Actually per GBATEK: at scanline y, base = ref + (y * PB, y * PD).
        // Then per pixel x: pos = base + (x * PA, x * PC).
        // X is added per dx; we start with (refx + y*PB) and step PA per pixel.
        int32_t xt = refx + static_cast<int32_t>(y) * pb;
        int32_t yt = refy + static_cast<int32_t>(y) * pd;
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            int32_t tex_x = xt >> 8;  // .8 fraction → integer
            int32_t tex_y = yt >> 8;
            xt += pa;
            yt += pc;

            if (wrap) {
                tex_x &= (bg_pixels - 1);
                tex_y &= (bg_pixels - 1);
            } else if (tex_x < 0 || tex_x >= bg_pixels ||
                       tex_y < 0 || tex_y >= bg_pixels) {
                continue;
            }

            int tile_x = tex_x >> 3;
            int tile_y = tex_y >> 3;
            int px_in_tile_x = tex_x & 7;
            int px_in_tile_y = tex_y & 7;

            // Affine BG tilemap is 1 byte per entry (8bpp tile index).
            uint32_t map_off = screen_base + tile_y * bg_tiles + tile_x;
            if (map_off >= 96u * 1024u) continue;
            uint8_t tile_index = vram[map_off];

            // 8bpp tile: 64 bytes (8x8 pixels, 1 byte each).
            uint32_t tile_addr = char_base + tile_index * 64u +
                                 px_in_tile_y * 8 + px_in_tile_x;
            if (tile_addr >= 96u * 1024u) continue;
            uint8_t pal_idx = vram[tile_addr];
            if (pal_idx == 0) continue;  // transparent

            uint16_t color = static_cast<uint16_t>(
                pal[pal_idx * 2] | (pal[pal_idx * 2 + 1] << 8));
            uint8_t* dst = rgb + (y * kScreenWidth + x) * 3;
            // Convert BGR555 → RGB888 inline.
            uint8_t r5 = color & 0x1F;
            uint8_t g5 = (color >> 5) & 0x1F;
            uint8_t b5 = (color >> 10) & 0x1F;
            dst[0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
            dst[1] = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
            dst[2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
        }
    }
}

}  // namespace

void GbaPpu::render(uint8_t* rgb,
                    uint16_t dispcnt,
                    const uint8_t* io,
                    const uint8_t* vram,
                    const uint8_t* oam,
                    const uint8_t* pal) const {
    (void)oam;  // referenced below
    // Start with backdrop (palette entry 0) for every pixel. Backdrop
    // is the BG palette index 0, at PAL[0..1].
    uint16_t backdrop = load_u16_le(&pal[0]);
    uint8_t bd_rgb[3];
    to_rgb888(backdrop, bd_rgb);
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            uint8_t* p = rgb + (y * kScreenWidth + x) * 3;
            p[0] = bd_rgb[0]; p[1] = bd_rgb[1]; p[2] = bd_rgb[2];
        }
    }

    // Forced blank (DISPCNT bit 7): output all-white per hardware.
    if (dispcnt & 0x0080u) {
        std::memset(rgb, 0xFF, kFramebufferBytes);
        return;
    }

    // Render background layers BEFORE OBJ so sprites composite on top.
    // Phase 2.5 scope: BG3 affine for BG mode 2 (GBA BIOS Nintendo
    // logo intro uses this). Other BG configurations land later.
    uint32_t bg_mode = dispcnt & 0x07u;
    bool bg3_enabled = (dispcnt & 0x0800u) != 0;
    if (bg3_enabled && (bg_mode == 1 || bg_mode == 2)) {
        // BG3 controls: BG3CNT at 0x0E, params at 0x30..0x3F.
        render_affine_bg(rgb, io, vram, pal,
                         0x0E, 0x30,
                         kScreenWidth, kScreenHeight);
    }

    // OBJ disabled?
    bool obj_enabled = (dispcnt & 0x1000u) != 0;
    if (!obj_enabled) return;

    // OBJ tile data starts at VRAM 0x10000 in tile modes (0/1/2) and
    // at VRAM 0x14000 in bitmap modes (3/4/5).
    uint32_t obj_tile_base = (bg_mode >= 3) ? 0x14000u : 0x10000u;
    bool obj_1d_mapping = (dispcnt & 0x0040u) != 0;

    // OBJ palette starts at PAL[0x200..0x3FF].
    const uint8_t* obj_pal = pal + 0x200;

    // Walk 128 OAM entries in priority-table order. For Phase 2.4 we
    // ignore priority bits and just back-to-front-draw so higher
    // OAM indices appear on top — close enough for the BIOS intro.
    for (int idx = 127; idx >= 0; --idx) {
        const uint8_t* entry = oam + idx * 8;
        uint16_t attr0 = load_u16_le(&entry[0]);
        uint16_t attr1 = load_u16_le(&entry[2]);
        uint16_t attr2 = load_u16_le(&entry[4]);

        bool rot_scale = (attr0 & 0x0100u) != 0;
        bool disable_or_double = (attr0 & 0x0200u) != 0;
        // DEBUG: render disabled sprites too to surface intro state.
        // The BIOS leaves the Nintendo logo sprites in OAM with the
        // disable flag set after the intro animation completes.
        // For Phase 2 visualization we ignore that flag — proper
        // gating returns in Phase 2.6.
        (void)disable_or_double;

        // Skip rot/scale sprites in this pass (TODO Phase 2.5).
        if (rot_scale) continue;

        uint32_t shape = (attr0 >> 14) & 0x3u;
        if (shape >= 3) continue;
        uint32_t size  = (attr1 >> 14) & 0x3u;
        int sw = kSpriteWH[shape][size][0];
        int sh = kSpriteWH[shape][size][1];

        int sy = static_cast<int>(attr0 & 0xFFu);
        int sx = static_cast<int>(attr1 & 0x1FFu);
        // Y wraps at 256 (GBATEK).
        if (sy >= 160) sy -= 256;
        // X is 9 bits signed.
        if (sx & 0x100) sx -= 0x200;

        bool hflip = (attr1 & 0x1000u) != 0;
        bool vflip = (attr1 & 0x2000u) != 0;
        bool color256 = (attr0 & 0x2000u) != 0;

        uint32_t tile_num = attr2 & 0x3FFu;
        uint32_t palette_bank = (attr2 >> 12) & 0xFu;

        // For 1D mapping each row of tiles is contiguous in VRAM.
        // For 2D mapping each row of OBJ tiles is 32 tiles wide.
        int tiles_w = sw / 8;
        int tiles_h = sh / 8;

        for (int ty = 0; ty < tiles_h; ++ty) {
            for (int tx = 0; tx < tiles_w; ++tx) {
                // Compute the source tile index, taking flips into
                // account (flipping the tile *layout* on top of
                // per-pixel flip).
                int src_tx = hflip ? (tiles_w - 1 - tx) : tx;
                int src_ty = vflip ? (tiles_h - 1 - ty) : ty;

                uint32_t this_tile;
                if (obj_1d_mapping) {
                    this_tile = tile_num + (src_ty * tiles_w + src_tx) *
                                                (color256 ? 2 : 1);
                } else {
                    // 2D mapping: the OBJ tile area is always 32
                    // *slots* wide regardless of color depth. Each
                    // 4bpp tile occupies 1 slot; each 8bpp tile
                    // occupies 2 horizontally-adjacent slots.
                    // Row stride in slot units is always 32.
                    this_tile = tile_num + (src_ty * 32u) +
                                src_tx * (color256 ? 2u : 1u);
                }
                // OBJ tile numbers are in 32-byte *slot* units
                // regardless of color depth. An 8bpp visible tile
                // (64 bytes) occupies 2 consecutive slots.
                uint32_t tile_off = obj_tile_base + this_tile * 32u;

                // Per-pixel emit.
                for (int py = 0; py < 8; ++py) {
                    int screen_y = sy + ty * 8 + py;
                    if (screen_y < 0 || screen_y >= static_cast<int>(kScreenHeight)) continue;
                    int src_py = vflip ? (7 - py) : py;
                    for (int px = 0; px < 8; ++px) {
                        int screen_x = sx + tx * 8 + px;
                        if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                        int src_px = hflip ? (7 - px) : px;

                        uint8_t pal_index;
                        if (color256) {
                            // 1 byte per pixel.
                            uint32_t off = tile_off + src_py * 8 + src_px;
                            if (off + 1 > 96 * 1024) continue;
                            pal_index = vram[off];
                        } else {
                            // 4bpp: 4 bytes per row, low nibble = even
                            // pixel, high nibble = odd pixel.
                            uint32_t off = tile_off + src_py * 4 + (src_px / 2);
                            if (off + 1 > 96 * 1024) continue;
                            uint8_t b = vram[off];
                            pal_index = (src_px & 1) ? (b >> 4) : (b & 0x0F);
                            if (pal_index == 0) continue;  // transparent
                            pal_index |= (palette_bank << 4);
                        }
                        if (color256 && pal_index == 0) continue;

                        uint16_t color = load_u16_le(&obj_pal[pal_index * 2]);
                        uint8_t* dst = rgb +
                            (screen_y * kScreenWidth + screen_x) * 3;
                        to_rgb888(color, dst);
                    }
                }
            }
        }
    }
}

}  // namespace gba
