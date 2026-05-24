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
    has_latched_fb_ = false;
    std::memset(latched_fb_.data(), 0xFF, latched_fb_.size());
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

uint32_t GbaPpu::cycles_until_next_event() const {
    uint32_t target_dot = (dot_in_scanline_ < kDotsVisible)
        ? kDotsVisible
        : kDotsPerScanline;
    uint32_t cycles = (target_dot - dot_in_scanline_) * kCyclesPerDot;
    if (cycles > cycle_in_dot_) cycles -= cycle_in_dot_;
    else cycles = 1;
    return cycles ? cycles : 1;
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

inline void blend_alpha_rgb888(const uint8_t* top,
                               const uint8_t* bottom,
                               uint32_t eva,
                               uint32_t evb,
                               uint8_t* out) {
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    for (int i = 0; i < 3; ++i) {
        uint32_t v = (static_cast<uint32_t>(top[i]) * eva +
                      static_cast<uint32_t>(bottom[i]) * evb) >> 4;
        if (v > 255) v = 255;
        out[i] = static_cast<uint8_t>(v);
    }
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
    if (v & 0x08000000u) v |= 0xF0000000u;
    return static_cast<int32_t>(v);
}

namespace {

bool obj_texel_opaque(const uint8_t* vram,
                      uint32_t obj_tile_base,
                      bool obj_1d_mapping,
                      uint32_t tile_num,
                      int tiles_w,
                      bool color256,
                      int tex_x,
                      int tex_y) {
    int tile_x_in_sprite = tex_x >> 3;
    int tile_y_in_sprite = tex_y >> 3;
    int px_in_tile       = tex_x & 7;
    int py_in_tile       = tex_y & 7;

    uint32_t this_tile;
    if (obj_1d_mapping) {
        this_tile = tile_num + (tile_y_in_sprite * tiles_w + tile_x_in_sprite) *
                                (color256 ? 2u : 1u);
    } else {
        this_tile = tile_num + (tile_y_in_sprite * 32u) +
                    tile_x_in_sprite * (color256 ? 2u : 1u);
    }
    uint32_t tile_off = obj_tile_base + this_tile * 32u;
    if (color256) {
        uint32_t off = tile_off + py_in_tile * 8 + px_in_tile;
        if (off + 1 > 96u * 1024u) return false;
        return vram[off] != 0;
    }
    uint32_t off = tile_off + py_in_tile * 4 + (px_in_tile / 2);
    if (off + 1 > 96u * 1024u) return false;
    uint8_t b = vram[off];
    uint8_t pal_index = (px_in_tile & 1) ? (b >> 4) : (b & 0x0F);
    return pal_index != 0;
}

void mark_obj_window_scanline(bool* mask,
                              uint32_t y,
                              uint16_t dispcnt,
                              const uint8_t* vram,
                              const uint8_t* oam,
                              uint32_t kScreenWidth,
                              uint32_t kScreenHeight) {
    if (y >= kScreenHeight) return;
    if ((dispcnt & 0x8000u) == 0) return;

    uint32_t bg_mode = dispcnt & 0x07u;
    uint32_t obj_tile_base = (bg_mode >= 3) ? 0x14000u : 0x10000u;
    bool obj_1d_mapping = (dispcnt & 0x0040u) != 0;

    for (int idx = 127; idx >= 0; --idx) {
        const uint8_t* entry = oam + idx * 8;
        uint16_t attr0 = load_u16_le(&entry[0]);
        uint16_t attr1 = load_u16_le(&entry[2]);
        uint16_t attr2 = load_u16_le(&entry[4]);

        bool rot_scale = (attr0 & 0x0100u) != 0;
        bool disable_or_double = (attr0 & 0x0200u) != 0;
        if (!rot_scale && disable_or_double) continue;
        uint32_t obj_mode = (attr0 >> 10) & 0x3u;
        if (obj_mode != 2) continue;

        uint32_t shape = (attr0 >> 14) & 0x3u;
        if (shape >= 3) continue;
        uint32_t size  = (attr1 >> 14) & 0x3u;
        int sw = kSpriteWH[shape][size][0];
        int sh = kSpriteWH[shape][size][1];

        int sy = static_cast<int>(attr0 & 0xFFu);
        int sx = static_cast<int>(attr1 & 0x1FFu);
        if (sy >= 160) sy -= 256;
        if (sx & 0x100) sx -= 0x200;

        bool color256 = (attr0 & 0x2000u) != 0;
        uint32_t tile_num = attr2 & 0x3FFu;
        int tiles_w = sw / 8;
        int tiles_h = sh / 8;

        if (rot_scale) {
            int bw = disable_or_double ? sw * 2 : sw;
            int bh = disable_or_double ? sh * 2 : sh;
            int j = static_cast<int>(y) - sy;
            if (j < 0 || j >= bh) continue;

            int affine_group = (attr1 >> 9) & 0x1Fu;
            const uint8_t* ag = oam + affine_group * 0x20u;
            int32_t pa = read_s16(ag, 0x06);
            int32_t pb = read_s16(ag, 0x0E);
            int32_t pc = read_s16(ag, 0x16);
            int32_t pd = read_s16(ag, 0x1E);

            int half_bw = bw >> 1;
            int half_bh = bh >> 1;
            int half_sw = sw >> 1;
            int half_sh = sh >> 1;
            int dy = j - half_bh;
            for (int i = 0; i < bw; ++i) {
                int screen_x = sx + i;
                if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                int dx = i - half_bw;
                int tex_x = ((pa * dx + pb * dy) >> 8) + half_sw;
                int tex_y = ((pc * dx + pd * dy) >> 8) + half_sh;
                if (tex_x < 0 || tex_x >= sw) continue;
                if (tex_y < 0 || tex_y >= sh) continue;
                if (obj_texel_opaque(vram, obj_tile_base, obj_1d_mapping,
                                     tile_num, tiles_w, color256,
                                     tex_x, tex_y)) {
                    mask[screen_x] = true;
                }
            }
            continue;
        }

        int line = static_cast<int>(y) - sy;
        if (line < 0 || line >= sh) continue;
        bool hflip = (attr1 & 0x1000u) != 0;
        bool vflip = (attr1 & 0x2000u) != 0;
        int ty = line >> 3;
        int py = line & 7;
        int src_ty = vflip ? (tiles_h - 1 - ty) : ty;
        int src_py = vflip ? (7 - py) : py;
        for (int tx = 0; tx < tiles_w; ++tx) {
            int src_tx = hflip ? (tiles_w - 1 - tx) : tx;
            for (int px = 0; px < 8; ++px) {
                int screen_x = sx + tx * 8 + px;
                if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                int src_px = hflip ? (7 - px) : px;
                int tex_x = src_tx * 8 + src_px;
                int tex_y = src_ty * 8 + src_py;
                if (obj_texel_opaque(vram, obj_tile_base, obj_1d_mapping,
                                     tile_num, tiles_w, color256,
                                     tex_x, tex_y)) {
                    mask[screen_x] = true;
                }
            }
        }
    }
}

void render_scanline_internal(uint8_t* rgb,
                              uint32_t y,
                              uint16_t dispcnt,
                              const uint8_t* io,
                              const uint8_t* vram,
                              const uint8_t* oam,
                              const uint8_t* pal,
                              uint32_t kScreenWidth,
                              uint32_t kScreenHeight) {
    if (y >= kScreenHeight) return;

    uint8_t* row = rgb + y * kScreenWidth * 3;
    if (dispcnt & 0x0080u) {
        std::memset(row, 0xFF, kScreenWidth * 3);
        return;
    }

    struct PixelCandidate {
        uint8_t rgb[3] = {0, 0, 0};
        int key = 0x7FFFFFFF;
        uint8_t layer = 5;
        bool target1 = false;
        bool target2 = false;
        bool valid = false;
    };

    uint16_t winout = static_cast<uint16_t>(io[0x4A] | (io[0x4B] << 8));
    bool obj_window_storage[GbaPpu::kScreenWidth] = {};
    bool* obj_window_mask = nullptr;
    if (dispcnt & 0x8000u) {
        mark_obj_window_scanline(obj_window_storage, y, dispcnt, vram, oam,
                                 kScreenWidth, kScreenHeight);
        obj_window_mask = obj_window_storage;
    }
    auto window_control = [&](uint32_t x) -> uint16_t {
        if (!obj_window_mask) return 0x3Fu;
        return obj_window_mask[x]
            ? static_cast<uint16_t>((winout >> 8) & 0x3Fu)
            : static_cast<uint16_t>(winout & 0x3Fu);
    };
    auto layer_enabled = [&](uint32_t x, uint32_t layer_bit) -> bool {
        return (window_control(x) & (1u << layer_bit)) != 0;
    };
    auto blend_enabled = [&](uint32_t x) -> bool {
        return (window_control(x) & (1u << 5)) != 0;
    };

    uint16_t bldcnt = static_cast<uint16_t>(io[0x50] | (io[0x51] << 8));
    uint16_t bldalpha = static_cast<uint16_t>(io[0x52] | (io[0x53] << 8));
    uint32_t first_targets = bldcnt & 0x3Fu;
    uint32_t second_targets = (bldcnt >> 8) & 0x3Fu;
    uint32_t effect = (bldcnt >> 6) & 0x3u;

    PixelCandidate top[GbaPpu::kScreenWidth];
    PixelCandidate second[GbaPpu::kScreenWidth];
    uint8_t backdrop_rgb[3];
    to_rgb888(load_u16_le(&pal[0]), backdrop_rgb);
    for (uint32_t x = 0; x < kScreenWidth; ++x) {
        top[x].rgb[0] = backdrop_rgb[0];
        top[x].rgb[1] = backdrop_rgb[1];
        top[x].rgb[2] = backdrop_rgb[2];
        top[x].key = 0x70000000;
        top[x].layer = 5;
        top[x].target1 = blend_enabled(x) && ((first_targets & (1u << 5)) != 0);
        top[x].target2 = (second_targets & (1u << 5)) != 0;
        top[x].valid = true;
    }
    auto submit = [&](uint32_t x,
                      const uint8_t* rgbv,
                      int key,
                      uint8_t layer,
                      bool target1,
                      bool target2) {
        PixelCandidate cand;
        cand.rgb[0] = rgbv[0];
        cand.rgb[1] = rgbv[1];
        cand.rgb[2] = rgbv[2];
        cand.key = key;
        cand.layer = layer;
        cand.target1 = target1;
        cand.target2 = target2;
        cand.valid = true;
        if (key < top[x].key) {
            second[x] = top[x];
            top[x] = cand;
        } else if (key < second[x].key) {
            second[x] = cand;
        }
    };

    uint32_t bg_mode = dispcnt & 0x07u;
    auto render_regular_bg = [&](uint32_t layer,
                                 uint32_t cnt_off,
                                 uint32_t scroll_off) {
        if ((dispcnt & (0x0100u << layer)) == 0) return;

        uint16_t bgcnt = static_cast<uint16_t>(
            io[cnt_off] | (io[cnt_off + 1] << 8));
        uint32_t char_base = ((bgcnt >> 2) & 0x3u) * 0x4000u;
        uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        bool color256 = (bgcnt & 0x0080u) != 0;
        uint32_t size_code = (bgcnt >> 14) & 0x3u;
        uint32_t bg_priority = bgcnt & 0x3u;
        uint32_t hofs = static_cast<uint16_t>(
            io[scroll_off] | (io[scroll_off + 1] << 8)) & 0x01FFu;
        uint32_t vofs = static_cast<uint16_t>(
            io[scroll_off + 2] | (io[scroll_off + 3] << 8)) & 0x01FFu;

        uint32_t width_tiles = (size_code & 1u) ? 64u : 32u;
        uint32_t height_tiles = (size_code & 2u) ? 64u : 32u;
        uint32_t width_px = width_tiles * 8u;
        uint32_t height_px = height_tiles * 8u;
        uint32_t block_cols = width_tiles / 32u;

        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            if (!layer_enabled(x, layer)) continue;
            uint32_t tex_x = (x + hofs) & (width_px - 1u);
            uint32_t tex_y = (y + vofs) & (height_px - 1u);
            uint32_t tile_x = tex_x >> 3;
            uint32_t tile_y = tex_y >> 3;
            uint32_t block = (tile_x >> 5) + (tile_y >> 5) * block_cols;
            uint32_t map_off = screen_base + block * 0x800u +
                ((tile_y & 31u) * 32u + (tile_x & 31u)) * 2u;
            if (map_off + 1 >= 96u * 1024u) continue;

            uint16_t entry = load_u16_le(&vram[map_off]);
            uint32_t tile_num = entry & 0x03FFu;
            bool hflip = (entry & 0x0400u) != 0;
            bool vflip = (entry & 0x0800u) != 0;
            uint32_t palette_bank = (entry >> 12) & 0x0Fu;
            uint32_t px = tex_x & 7u;
            uint32_t py = tex_y & 7u;
            if (hflip) px = 7u - px;
            if (vflip) py = 7u - py;

            uint8_t pal_idx = 0;
            if (color256) {
                uint32_t tile_addr = char_base + tile_num * 64u + py * 8u + px;
                if (tile_addr >= 96u * 1024u) continue;
                pal_idx = vram[tile_addr];
            } else {
                uint32_t tile_addr = char_base + tile_num * 32u +
                    py * 4u + (px >> 1);
                if (tile_addr >= 96u * 1024u) continue;
                uint8_t packed = vram[tile_addr];
                pal_idx = (px & 1u) ? (packed >> 4) : (packed & 0x0Fu);
                pal_idx = static_cast<uint8_t>(
                    pal_idx | static_cast<uint8_t>(palette_bank << 4));
            }
            if ((pal_idx & (color256 ? 0xFFu : 0x0Fu)) == 0) continue;

            uint8_t rgbv[3];
            to_rgb888(load_u16_le(&pal[pal_idx * 2]), rgbv);
            submit(x, rgbv,
                   static_cast<int>(bg_priority * 128u + 128u + layer),
                   static_cast<uint8_t>(layer),
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };

    auto render_affine_bg = [&](uint32_t layer,
                                uint32_t cnt_off,
                                uint32_t param_off) {
        if ((dispcnt & (0x0100u << layer)) == 0) return;

        uint16_t bgcnt = static_cast<uint16_t>(io[cnt_off] |
                                               (io[cnt_off + 1] << 8));
        uint32_t char_base   = ((bgcnt >> 2) & 0x3u) * 0x4000u;
        uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        bool wrap            = (bgcnt & 0x2000u) != 0;
        uint32_t size_code   = (bgcnt >> 14) & 0x3u;
        int bg_pixels = 128 << size_code;
        int bg_tiles  = bg_pixels / 8;
        int bg_priority = static_cast<int>(bgcnt & 0x3u);

        int32_t pa = read_s16(io, param_off + 0x00);
        int32_t pb = read_s16(io, param_off + 0x02);
        int32_t pc = read_s16(io, param_off + 0x04);
        int32_t pd = read_s16(io, param_off + 0x06);
        int32_t refx = read_s28_ref(io, param_off + 0x08);
        int32_t refy = read_s28_ref(io, param_off + 0x0C);
        int32_t xt = refx + static_cast<int32_t>(y) * pb;
        int32_t yt = refy + static_cast<int32_t>(y) * pd;
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            int32_t tex_x = xt >> 8;
            int32_t tex_y = yt >> 8;
            xt += pa;
            yt += pc;
            if (!layer_enabled(x, layer)) continue;
            if (wrap) {
                tex_x &= (bg_pixels - 1);
                tex_y &= (bg_pixels - 1);
            } else if (tex_x < 0 || tex_x >= bg_pixels ||
                       tex_y < 0 || tex_y >= bg_pixels) {
                continue;
            }
            uint32_t map_off = screen_base + (tex_y >> 3) * bg_tiles + (tex_x >> 3);
            if (map_off >= 96u * 1024u) continue;
            uint8_t tile_index = vram[map_off];
            uint32_t tile_addr = char_base + tile_index * 64u +
                                 (tex_y & 7) * 8 + (tex_x & 7);
            if (tile_addr >= 96u * 1024u) continue;
            uint8_t pal_idx = vram[tile_addr];
            if (pal_idx == 0) continue;
            uint8_t rgbv[3];
            to_rgb888(load_u16_le(&pal[pal_idx * 2]), rgbv);
            submit(x, rgbv,
                   static_cast<int>(bg_priority * 128 + 128 + layer),
                   static_cast<uint8_t>(layer),
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };

    if (bg_mode == 0) {
        render_regular_bg(3, 0x0E, 0x1C);
        render_regular_bg(2, 0x0C, 0x18);
        render_regular_bg(1, 0x0A, 0x14);
        render_regular_bg(0, 0x08, 0x10);
    } else if (bg_mode == 1) {
        render_affine_bg(2, 0x0C, 0x20);
        render_regular_bg(1, 0x0A, 0x14);
        render_regular_bg(0, 0x08, 0x10);
    } else if (bg_mode == 2) {
        render_affine_bg(3, 0x0E, 0x30);
        render_affine_bg(2, 0x0C, 0x20);
    }

    if (dispcnt & 0x1000u) {
        uint32_t obj_tile_base = (bg_mode >= 3) ? 0x14000u : 0x10000u;
        bool obj_1d_mapping = (dispcnt & 0x0040u) != 0;
        const uint8_t* obj_pal = pal + 0x200;
        for (int idx = 127; idx >= 0; --idx) {
            const uint8_t* entry = oam + idx * 8;
            uint16_t attr0 = load_u16_le(&entry[0]);
            uint16_t attr1 = load_u16_le(&entry[2]);
            uint16_t attr2 = load_u16_le(&entry[4]);
            bool rot_scale = (attr0 & 0x0100u) != 0;
            bool disable_or_double = (attr0 & 0x0200u) != 0;
            if (!rot_scale && disable_or_double) continue;
            uint32_t obj_mode = (attr0 >> 10) & 0x3u;
            if (obj_mode == 2 || obj_mode == 3) continue;
            uint32_t shape = (attr0 >> 14) & 0x3u;
            if (shape >= 3) continue;
            uint32_t size  = (attr1 >> 14) & 0x3u;
            int sw = kSpriteWH[shape][size][0];
            int sh = kSpriteWH[shape][size][1];
            int sy = static_cast<int>(attr0 & 0xFFu);
            int sx = static_cast<int>(attr1 & 0x1FFu);
            if (sy >= 160) sy -= 256;
            if (sx & 0x100) sx -= 0x200;
            bool color256 = (attr0 & 0x2000u) != 0;
            uint32_t tile_num = attr2 & 0x3FFu;
            uint32_t palette_bank = (attr2 >> 12) & 0xFu;
            int tiles_w = sw / 8;
            int tiles_h = sh / 8;
            int priority = static_cast<int>((attr2 >> 10) & 0x3u);
            int key = priority * 128 + idx;
            bool obj_target2 = (second_targets & (1u << 4)) != 0;
            auto emit_obj = [&](int tex_x, int tex_y, int screen_x) {
                if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) return;
                if (!layer_enabled(static_cast<uint32_t>(screen_x), 4)) return;
                int tile_x_in_sprite = tex_x >> 3;
                int tile_y_in_sprite = tex_y >> 3;
                int px_in_tile = tex_x & 7;
                int py_in_tile = tex_y & 7;
                uint32_t this_tile;
                if (obj_1d_mapping) {
                    this_tile = tile_num + (tile_y_in_sprite * tiles_w + tile_x_in_sprite) *
                                            (color256 ? 2u : 1u);
                } else {
                    this_tile = tile_num + (tile_y_in_sprite * 32u) +
                                tile_x_in_sprite * (color256 ? 2u : 1u);
                }
                uint32_t tile_off = obj_tile_base + this_tile * 32u;
                uint8_t pal_index;
                if (color256) {
                    uint32_t off = tile_off + py_in_tile * 8 + px_in_tile;
                    if (off + 1 > 96u * 1024u) return;
                    pal_index = vram[off];
                    if (pal_index == 0) return;
                } else {
                    uint32_t off = tile_off + py_in_tile * 4 + (px_in_tile / 2);
                    if (off + 1 > 96u * 1024u) return;
                    uint8_t b = vram[off];
                    pal_index = (px_in_tile & 1) ? (b >> 4) : (b & 0x0F);
                    if (pal_index == 0) return;
                    pal_index = static_cast<uint8_t>(pal_index | (palette_bank << 4));
                }
                uint8_t rgbv[3];
                to_rgb888(load_u16_le(&obj_pal[pal_index * 2]), rgbv);
                uint32_t ux = static_cast<uint32_t>(screen_x);
                bool t1 = blend_enabled(ux) &&
                    obj_mode == 1;
                submit(ux, rgbv, key, 4, t1, obj_target2);
            };
            if (rot_scale) {
                int bw = disable_or_double ? sw * 2 : sw;
                int bh = disable_or_double ? sh * 2 : sh;
                int j = static_cast<int>(y) - sy;
                if (j < 0 || j >= bh) continue;
                int affine_group = (attr1 >> 9) & 0x1Fu;
                const uint8_t* ag = oam + affine_group * 0x20u;
                int32_t pa = read_s16(ag, 0x06);
                int32_t pb = read_s16(ag, 0x0E);
                int32_t pc = read_s16(ag, 0x16);
                int32_t pd = read_s16(ag, 0x1E);
                int half_bw = bw >> 1;
                int half_bh = bh >> 1;
                int half_sw = sw >> 1;
                int half_sh = sh >> 1;
                int dy = j - half_bh;
                for (int i = 0; i < bw; ++i) {
                    int dx = i - half_bw;
                    int tex_x = ((pa * dx + pb * dy) >> 8) + half_sw;
                    int tex_y = ((pc * dx + pd * dy) >> 8) + half_sh;
                    if (tex_x < 0 || tex_x >= sw) continue;
                    if (tex_y < 0 || tex_y >= sh) continue;
                    emit_obj(tex_x, tex_y, sx + i);
                }
                continue;
            }
            int line = static_cast<int>(y) - sy;
            if (line < 0 || line >= sh) continue;
            bool hflip = (attr1 & 0x1000u) != 0;
            bool vflip = (attr1 & 0x2000u) != 0;
            int ty = line >> 3;
            int py = line & 7;
            int src_ty = vflip ? (tiles_h - 1 - ty) : ty;
            int src_py = vflip ? (7 - py) : py;
            for (int tx = 0; tx < tiles_w; ++tx) {
                int src_tx = hflip ? (tiles_w - 1 - tx) : tx;
                for (int px = 0; px < 8; ++px) {
                    int src_px = hflip ? (7 - px) : px;
                    emit_obj(src_tx * 8 + src_px, src_ty * 8 + src_py,
                             sx + tx * 8 + px);
                }
            }
        }
    }

    for (uint32_t x = 0; x < kScreenWidth; ++x) {
        uint8_t* dst = row + x * 3;
        if (effect == 1 && (bldalpha & 0x1Fu) != 0 &&
            top[x].target1 && second[x].valid && second[x].target2 &&
            !(top[x].layer == 4 && second[x].layer == 4)) {
            blend_alpha_rgb888(top[x].rgb, second[x].rgb,
                               bldalpha & 0x1Fu,
                               (bldalpha >> 8) & 0x1Fu,
                               dst);
        } else {
            dst[0] = top[x].rgb[0];
            dst[1] = top[x].rgb[1];
            dst[2] = top[x].rgb[2];
        }
    }
    return;
}

}  // namespace

void GbaPpu::render(uint8_t* rgb,
                    uint16_t dispcnt,
                    const uint8_t* io,
                    const uint8_t* vram,
                    const uint8_t* oam,
                    const uint8_t* pal) const {
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        render_scanline_internal(rgb, y, dispcnt, io, vram, oam, pal,
                                 kScreenWidth, kScreenHeight);
    }
}

#if 0
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
        // For non-affine sprites bit 9 is the disable bit; for affine
        // sprites it's the double-size flag (handled in the affine
        // pass). Skip disabled non-affine sprites so the BIOS intro
        // screen blanks correctly once the BIOS clears the wordmark
        // by setting these bits.
        if (!rot_scale && disable_or_double) continue;

        // OBJ Mode (attr0 bits 10-11) per GBATEK § "GBA OBJs — OAM
        // Attribute 0":
        //   0 = Normal  (visible pixel)
        //   1 = Semi-Transparent (alpha-blend with BLDALPHA)
        //   2 = OBJ Window (the sprite's opaque pixels define a
        //       window region; sprite itself is NOT drawn)
        //   3 = Prohibited
        // Without this check the BIOS intro's OBJ-Window stencil
        // sprites leak into the visible frame as garbage pink pixels.
        uint32_t obj_mode = (attr0 >> 10) & 0x3u;
        if (obj_mode == 2) continue;            // window stencil — invisible
        if (obj_mode == 3) continue;            // prohibited
        // Semi-transparent (mode 1) renders as normal for now; proper
        // alpha blending lands when we wire BLDCNT / BLDALPHA.

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

        bool color256 = (attr0 & 0x2000u) != 0;
        uint32_t tile_num = attr2 & 0x3FFu;
        uint32_t palette_bank = (attr2 >> 12) & 0xFu;

        // For 1D mapping each row of tiles is contiguous in VRAM.
        // For 2D mapping each row of OBJ tiles is 32 tiles wide.
        int tiles_w = sw / 8;
        int tiles_h = sh / 8;

        // Texel sampler shared by affine + non-affine paths. Returns
        // false on transparent / out-of-VRAM, otherwise writes the
        // RGB888 pixel at (screen_x, screen_y).
        auto sample_and_emit = [&](int tex_x, int tex_y,
                                   int screen_x, int screen_y) {
            int tile_x_in_sprite = tex_x >> 3;
            int tile_y_in_sprite = tex_y >> 3;
            int px_in_tile       = tex_x & 7;
            int py_in_tile       = tex_y & 7;

            uint32_t this_tile;
            if (obj_1d_mapping) {
                this_tile = tile_num + (tile_y_in_sprite * tiles_w + tile_x_in_sprite) *
                                            (color256 ? 2u : 1u);
            } else {
                this_tile = tile_num + (tile_y_in_sprite * 32u) +
                            tile_x_in_sprite * (color256 ? 2u : 1u);
            }
            uint32_t tile_off = obj_tile_base + this_tile * 32u;

            uint8_t pal_index;
            if (color256) {
                uint32_t off = tile_off + py_in_tile * 8 + px_in_tile;
                if (off + 1 > 96u * 1024u) return;
                pal_index = vram[off];
                if (pal_index == 0) return;  // transparent
            } else {
                uint32_t off = tile_off + py_in_tile * 4 + (px_in_tile / 2);
                if (off + 1 > 96u * 1024u) return;
                uint8_t b = vram[off];
                pal_index = (px_in_tile & 1) ? (b >> 4) : (b & 0x0F);
                if (pal_index == 0) return;  // transparent
                pal_index = static_cast<uint8_t>(pal_index | (palette_bank << 4));
            }

            uint16_t color = load_u16_le(&obj_pal[pal_index * 2]);
            uint8_t* dst = rgb + (screen_y * kScreenWidth + screen_x) * 3;
            to_rgb888(color, dst);
        };

        if (rot_scale) {
            // Affine sprite. Bounding box is 2x the sprite size when
            // the double-size flag is set, otherwise = sprite size.
            int bw = disable_or_double ? sw * 2 : sw;
            int bh = disable_or_double ? sh * 2 : sh;

            int affine_group = (attr1 >> 9) & 0x1Fu;
            const uint8_t* ag = oam + affine_group * 0x20u;
            // PA/PB/PC/PD live at offsets 0x06, 0x0E, 0x16, 0x1E within
            // the 32-byte affine block (the other bytes belong to OBJ
            // attr0/1/2 entries that share the same 8-byte slots).
            int32_t pa = read_s16(ag, 0x06);
            int32_t pb = read_s16(ag, 0x0E);
            int32_t pc = read_s16(ag, 0x16);
            int32_t pd = read_s16(ag, 0x1E);

            int half_bw = bw >> 1;
            int half_bh = bh >> 1;
            int half_sw = sw >> 1;
            int half_sh = sh >> 1;

            for (int j = 0; j < bh; ++j) {
                int screen_y = sy + j;
                if (screen_y < 0 || screen_y >= static_cast<int>(kScreenHeight)) continue;
                int dy = j - half_bh;
                for (int i = 0; i < bw; ++i) {
                    int screen_x = sx + i;
                    if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                    int dx = i - half_bw;

                    // (tex_x, tex_y) = matrix * (dx, dy) + sprite_center.
                    int tex_x = ((pa * dx + pb * dy) >> 8) + half_sw;
                    int tex_y = ((pc * dx + pd * dy) >> 8) + half_sh;
                    if (tex_x < 0 || tex_x >= sw) continue;
                    if (tex_y < 0 || tex_y >= sh) continue;

                    sample_and_emit(tex_x, tex_y, screen_x, screen_y);
                }
            }
            continue;
        }

        // Non-affine sprite from here on.
        bool hflip = (attr1 & 0x1000u) != 0;
        bool vflip = (attr1 & 0x2000u) != 0;

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

#endif

void GbaPpu::render_scanline(uint32_t y,
                             uint16_t dispcnt,
                             const uint8_t* io,
                             const uint8_t* vram,
                             const uint8_t* oam,
                             const uint8_t* pal) {
    render_scanline_internal(latched_fb_.data(), y, dispcnt, io, vram, oam, pal,
                             kScreenWidth, kScreenHeight);
}

void GbaPpu::latch_framebuffer(uint16_t dispcnt,
                               const uint8_t* io,
                               const uint8_t* vram,
                               const uint8_t* oam,
                               const uint8_t* pal) {
    render(latched_fb_.data(), dispcnt, io, vram, oam, pal);
    has_latched_fb_ = true;
}

void GbaPpu::mark_framebuffer_latched() {
    has_latched_fb_ = true;
}

}  // namespace gba
