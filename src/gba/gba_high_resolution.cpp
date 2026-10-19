#include "gba_high_resolution.h"
#include "gba_color.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gba {
namespace {
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (unsigned(p[1]) << 8)); }
int64_t ref28(const uint8_t* p) {
    uint32_t n = uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16);
    return int32_t(n << 4) >> 4;
}
int64_t delta28(int64_t n) { return int32_t(uint32_t(n) << 4) >> 4; }
}
const char* GbaHighResolutionFrame::sampler_implementation() {
#if defined(GBARECOMP_HD_SAMPLER_HLE)
    return "HLE (cached indexed affine surface)";
#else
    return "LLE (direct tiled affine sampling)";
#endif
}
bool GbaHighResolutionFrame::Pixel::operator==(const Pixel& p) const {
    return color == p.color && key == p.key && layer == p.layer &&
        target1 == p.target1 && target2 == p.target2 && valid == p.valid;
}
bool GbaHighResolutionFrame::Affine::operator==(const Affine& a) const {
    return active == a.active && cnt == a.cnt && cache == a.cache && y == a.y &&
        x == a.x && v == a.v && pa == a.pa && pc == a.pc && dx == a.dx && dv == a.dv &&
        dpa == a.dpa && dpc == a.dpc;
}
void GbaHighResolutionFrame::Surface::update(uint16_t cnt, const uint8_t* vram) {
    const unsigned next_layout = cnt & 0xdf0c; // size, map and character bases
    const bool reset = layout != next_layout;
    size = 128u << (cnt >> 14);
    const auto* source_chars = vram + ((cnt >> 2) & 3) * 0x4000;
    const auto* source_map = vram + ((cnt >> 8) & 31) * 0x800;
    const unsigned tiles = size / 8, entries = tiles * tiles;
    if (!reset && std::memcmp(chars.data(), source_chars, chars.size()) == 0 &&
        std::memcmp(map.data(), source_map, entries) == 0) return;
    std::array<bool, 256> dirty{};
    for (unsigned tile = 0; tile < dirty.size(); ++tile)
        dirty[tile] = reset || std::memcmp(chars.data() + tile * 64,
                                         source_chars + tile * 64, 64) != 0;
    map.resize(entries); texels.resize(size_t(size) * size);
    for (unsigned entry = 0; entry < entries; ++entry) {
        const unsigned tile = source_map[entry];
        if (!reset && map[entry] == tile && !dirty[tile]) continue;
        auto* dst = texels.data() + (entry / tiles) * 8 * size + (entry % tiles) * 8;
        for (unsigned row = 0; row < 8; ++row)
            std::memcpy(dst + row * size, source_chars + tile * 64 + row * 8, 8);
    }
    std::memcpy(chars.data(), source_chars, chars.size());
    std::memcpy(map.data(), source_map, entries);
    layout = next_layout;
    ++version;
}
GbaHighResolutionFrame::GbaHighResolutionFrame(unsigned scale) : scale_(scale) {
    if (scale < 2 || scale > kMaxScale)
        throw std::invalid_argument("internal resolution must be from 1 through 16");
}
void GbaHighResolutionFrame::configure(unsigned width, unsigned height) {
    if (width == width_ && height == height_) return;
    if (width < 240 || width > 576 || height < 160 || height > 864)
        throw std::invalid_argument("invalid high-resolution presentation dimensions");
    width_ = width; height_ = height;
    top_.resize(width); second_.resize(width); windows_.resize(width); black_.resize(width);
    written_.resize(height);
    cached_rows_.clear(); cached_rows_.resize(height);
    work_.resize(size_t(width) * height * scale_ * scale_ * 3);
    latched_.resize(work_.size());
    scratch_.resize(size_t(width) * height * 3);
    invalidate();
}
void GbaHighResolutionFrame::invalidate() {
    complete_ = false;
    for (auto& surface : surfaces_) surface.layout = ~0u;
    begin_frame();
}
void GbaHighResolutionFrame::begin_frame() {
    previous_ = {};
    std::fill(written_.begin(), written_.end(), 0);
    written_lines_ = 0;
}
bool GbaHighResolutionFrame::begin_line(int y, unsigned ox, unsigned oy,
    uint16_t dispcnt, const uint8_t* io, const uint8_t* vram, const uint8_t* pal,
    HighResolutionAffineProvider provider, bool filter) {
    selected_ = 0; affine_ = {};
    if (!provider || y < 0 || y >= 160 || (dispcnt & 0x80)) {
        previous_ = {}; return false;
    }
    const unsigned mode = dispcnt & 7;
    for (unsigned i = 0; i < 2; ++i) {
        const unsigned layer = 2 + i;
        if (!(dispcnt & (0x100u << layer)) ||
            !(mode == 2 || (mode == 1 && layer == 2)) || !provider(layer, y)) {
            previous_[i] = {}; continue;
        }
        auto& a = affine_[i];
        const unsigned param = 0x20 + i * 0x10;
        a.active = true; a.y = y; a.cnt = u16(io + 0x0c + i * 2); a.cache = i;
        a.pa = int16_t(u16(io + param)); a.pc = int16_t(u16(io + param + 4));
        a.dx = int16_t(u16(io + param + 2)); a.dv = int16_t(u16(io + param + 6));
        // The PPU supplies its effective current reference, expressed through
        // the existing ref + y*PB/PD compositor interface (including DMA reloads).
        a.x = ref28(io + param + 8) + y * a.dx;
        a.v = ref28(io + param + 12) + y * a.dv;
        const auto& previous = previous_[i];
        if (previous.active && previous.y + 1 == y && previous.cnt == a.cnt) {
            // Presentation approximation: extrapolate within this hardware
            // scanline from adjacent published transforms. This avoids copying
            // future VRAM or rerunning HBlank DMA to invent subscanlines. Native
            // sample positions stay exact. Never interpolate across mode/layout
            // boundaries or a restored/partial raster.
            a.dx = delta28(a.x - previous.x); a.dv = delta28(a.v - previous.v);
            a.dpa = a.pa - previous.pa; a.dpc = a.pc - previous.pc;
        }
        previous_[i] = a;
        selected_ |= 1u << layer;
    }
    if (!selected_) return false;
    ++affine_lines_;
    ox_ = ox; row_ = unsigned(y + int(oy));
    vram_ = vram; pal_ = pal; filter_ = filter;
    bldcnt_ = u16(io + 0x50); alpha_ = u16(io + 0x52);
    brightness_ = std::min(16u, unsigned(u16(io + 0x54) & 31));
    const Pixel backdrop{u16(pal), 0x70000000, 5,
                         bool(bldcnt_ & 0x20), bool(bldcnt_ & 0x2000), true};
    std::fill(top_.begin(), top_.end(), backdrop);
    std::fill(second_.begin(), second_.end(), Pixel{});
    std::fill(windows_.begin(), windows_.end(), 0x3f);
    std::fill(black_.begin(), black_.end(), 0);
    return true;
}
void GbaHighResolutionFrame::window(unsigned x, uint8_t control) {
    windows_[x] = control;
    top_[x].target1 = (control & 0x20) && (bldcnt_ & 0x20);
}
void GbaHighResolutionFrame::insert(Pixel& top, Pixel& second, const Pixel& p) {
    if (p.key < top.key) { second = top; top = p; }
    else if (p.key < second.key) second = p;
}
void GbaHighResolutionFrame::submit(unsigned x, uint16_t color, int key,
    uint8_t layer, bool target1, bool target2) {
    if (selected_ & (1u << layer)) return;
    insert(top_[x], second_[x], {color, key, layer, target1, target2, true});
}
// Keep the cached sample inside the subpixel loop so the compiler can hoist
// the row's fixed palette/surface/layout inputs and avoid a call per sample.
// Other compilers retain the same portable implementation without this hint.
#if defined(GBARECOMP_HD_SAMPLER_HLE) && defined(_MSC_VER)
__forceinline
#elif defined(GBARECOMP_HD_SAMPLER_HLE) && (defined(__GNUC__) || defined(__clang__))
__attribute__((always_inline)) inline
#endif
bool GbaHighResolutionFrame::sample(const Affine& a, int64_t u, int64_t v,
                                    uint16_t& color) const {
    const unsigned size = 128u << (a.cnt >> 14);
#if defined(GBARECOMP_HD_SAMPLER_HLE)
    int64_t x = u >> 8, y = v >> 8;
    const bool wrap = a.cnt & 0x2000;
    if (wrap) { x &= size - 1; y &= size - 1; }
    else if (x < 0 || y < 0 || x >= size || y >= size) return false;
    const auto* texels = surfaces_[a.cache].texels.data();
    const unsigned c00 = texels[size_t(y) * size + size_t(x)];
    if (!c00) return false;
    color = palette_[c00];
    if (!filter_) return true;
    unsigned x1 = unsigned(x + 1), y1 = unsigned(y + 1);
    if (wrap) { x1 &= size - 1; y1 &= size - 1; }
    else if (x1 == size || y1 == size) return true;
    const unsigned c10 = texels[size_t(y) * size + x1];
    const unsigned c01 = texels[size_t(y1) * size + size_t(x)];
    const unsigned c11 = texels[size_t(y1) * size + x1];
    if (!c10 || !c01 || !c11) return true;
    const unsigned fx = unsigned(u) & 255, fy = unsigned(v) & 255;
    // Three independent 21-bit lanes fit in uint64_t. Each weighted channel
    // sums to at most 31*65536; adding 32768 still cannot carry into its
    // neighbor. This performs all three exact integer filters in four
    // portable multiplies, including the original rounding and bit-15 loss.
    constexpr uint64_t round = (uint64_t(1) | (uint64_t(1) << 21) | (uint64_t(1) << 42)) * 32768;
    const uint64_t sum = filter_palette_[c00] * ((256 - fx) * (256 - fy)) +
                         filter_palette_[c10] * (fx * (256 - fy)) +
                         filter_palette_[c01] * ((256 - fx) * fy) +
                         filter_palette_[c11] * (fx * fy) + round;
    color = uint16_t(((sum >> 16) & 31) | (((sum >> 37) & 31) << 5) | (((sum >> 58) & 31) << 10));
    return true;
#else
    const unsigned chars = ((a.cnt >> 2) & 3) * 0x4000;
    const unsigned map = ((a.cnt >> 8) & 31) * 0x800;
    auto texel = [&](int64_t x, int64_t y, uint16_t& c) {
        if (a.cnt & 0x2000) { x &= size - 1; y &= size - 1; }
        else if (x < 0 || y < 0 || x >= size || y >= size) return false;
        const unsigned map_entry = map + unsigned(y >> 3) * (size >> 3) + unsigned(x >> 3);
        if (map_entry >= 96 * 1024) return false;
        const unsigned tile = vram_[map_entry];
        const unsigned address = chars + tile * 64 + (unsigned(y) & 7) * 8 + (unsigned(x) & 7);
        if (address >= 96 * 1024) return false;
        const unsigned index = vram_[address];
        if (!index) return false;
        c = u16(pal_ + index * 2); return true;
    };
    const int64_t x = u >> 8, y = v >> 8;
    if (!texel(x, y, color)) return false;
    if (!filter_) return true;
    uint16_t c10, c01, c11;
    if (!texel(x + 1, y, c10) || !texel(x, y + 1, c01) || !texel(x + 1, y + 1, c11)) return true;
    const unsigned fx = unsigned(u) & 255, fy = unsigned(v) & 255;
    const unsigned weights[] = {(256 - fx) * (256 - fy), fx * (256 - fy), (256 - fx) * fy, fx * fy};
    const uint16_t colors[] = {color, c10, c01, c11};
    uint16_t result = 0;
    for (unsigned shift : {0u, 5u, 10u}) {
        unsigned sum = 0;
        for (unsigned i = 0; i < 4; ++i) sum += ((colors[i] >> shift) & 31) * weights[i];
        result |= uint16_t(((sum + 0x8000) >> 16) << shift);
    }
    color = result; return true;
#endif
}
void GbaHighResolutionFrame::write_color(uint8_t* dst, const Pixel& top,
                                        const Pixel& second) const {
    const unsigned effect = (bldcnt_ >> 6) & 3;
    const bool alpha = (effect == 1 || top.layer == 4) && top.target1 &&
        second.valid && second.target2 && !(top.layer == 4 && second.layer == 4);
    uint16_t c = top.color;
    if (alpha) c = color::blend_alpha_gba555(c, second.color, alpha_ & 31, (alpha_ >> 8) & 31);
    else if (top.target1 && effect == 2) c = color::brighten_gba555(c, brightness_);
    else if (top.target1 && effect == 3) c = color::darken_gba555(c, brightness_);
    color::to_rgb888(c, dst);
}
void GbaHighResolutionFrame::wrote_line(unsigned y) {
    if (y < height_ && !written_[y]) { written_[y] = 1; ++written_lines_; }
}
void GbaHighResolutionFrame::resolve() {
#if defined(GBARECOMP_HD_SAMPLER_HLE)
    for (const auto& a : affine_) if (a.active) surfaces_[a.cache].update(a.cnt, vram_);
    for (unsigned i = 0; i < palette_.size(); ++i) {
        const uint16_t c = palette_[i] = u16(pal_ + i * 2);
        filter_palette_[i] = (c & 31) | (uint64_t((c >> 5) & 31) << 21) | (uint64_t((c >> 10) & 31) << 42);
    }
    auto& cached = cached_rows_[row_];
    const std::array<uint64_t, 2> versions{
        affine_[0].active ? surfaces_[0].version : 0,
        affine_[1].active ? surfaces_[1].version : 0};
    const bool reuse = complete_ && cached.frame == frames_ && cached.versions == versions &&
        cached.affine == affine_ && cached.palette == palette_ && cached.ox == ox_ &&
        cached.bldcnt == bldcnt_ && cached.alpha == alpha_ && cached.brightness == brightness_ &&
        cached.filter == filter_ && cached.top == top_ && cached.second == second_ &&
        cached.windows == windows_ && cached.black == black_;
    // Stamp work for the next completed latch, never for the currently
    // published pixels. Repeated/partial rows cannot use freshly changed
    // metadata to copy an older published raster. Equality is field-wise;
    // no hashes, padding bytes, host timing or stale guest pointers decide a hit.
    cached.frame = frames_ + 1;
    if (reuse) {
        const size_t offset = size_t(row_) * scale_ * width() * 3;
        std::memcpy(work_.data() + offset, latched_.data() + offset, size_t(scale_) * width() * 3);
        ++reused_lines_; wrote_line(row_); return;
    }
    cached.versions = versions; cached.affine = affine_; cached.palette = palette_;
    cached.ox = ox_; cached.bldcnt = bldcnt_; cached.alpha = alpha_;
    cached.brightness = brightness_; cached.filter = filter_;
    cached.top = top_; cached.second = second_;
    cached.windows = windows_; cached.black = black_;
#endif
    for (unsigned sy = 0; sy < scale_; ++sy) {
        struct Row {
            int64_t u = 0, v = 0, pa = 0, pc = 0;
            std::array<int64_t, kMaxScale> du{}, dv{};
        };
        std::array<Row, 2> rows{};
        for (unsigned i = 0; i < 2; ++i) {
            const auto& a = affine_[i];
            if (!a.active) continue;
            auto& r = rows[i];
            // Keep each signed division's truncation exactly as in the scalar
            // formula, but perform it once per subline instead of per pixel.
            r.pa = a.pa + a.dpa * sy / scale_;
            r.pc = a.pc + a.dpc * sy / scale_;
            r.u = a.x + a.dx * sy / scale_ - int64_t(ox_) * r.pa;
            r.v = a.v + a.dv * sy / scale_ - int64_t(ox_) * r.pc;
            for (unsigned sx = 0; sx < scale_; ++sx) {
                r.du[sx] = r.pa * sx / scale_;
                r.dv[sx] = r.pc * sx / scale_;
            }
        }
        auto* dst = work_.data() + size_t(row_ * scale_ + sy) * width() * 3;
        for (unsigned x = 0; x < width_; ++x) {
            for (unsigned sx = 0; sx < scale_; ++sx, dst += 3) {
                if (black_[x]) { dst[0] = dst[1] = dst[2] = 0; continue; }
                Pixel top = top_[x], second = second_[x];
                for (unsigned i = 0; i < 2; ++i) {
                    const auto& a = affine_[i];
                    const unsigned layer = i + 2;
                    if (!a.active || !(windows_[x] & (1u << layer))) continue;
                    const auto& r = rows[i];
                    uint16_t color;
                    ++samples_;
                    if (!sample(a, r.u + r.du[sx], r.v + r.dv[sx], color)) continue;
                    insert(top, second, {color, int((a.cnt & 3) * 256 + 128 + layer), uint8_t(layer),
                        bool((windows_[x] & 0x20) && (bldcnt_ & (1u << layer))),
                        bool(bldcnt_ & (0x100u << layer)), true});
                }
                write_color(dst, top, second);
            }
            for (auto& r : rows) { r.u += r.pa; r.v += r.pc; }
        }
    }
    wrote_line(row_);
}
void GbaHighResolutionFrame::upscale_line(unsigned y, const uint8_t* native_row) {
#if defined(GBARECOMP_HD_SAMPLER_HLE)
    cached_rows_[y].frame = ~uint64_t(0);
#endif
    for (unsigned sy = 0; sy < scale_; ++sy) {
        auto* dst = work_.data() + size_t(y * scale_ + sy) * width() * 3;
#if defined(GBARECOMP_HD_SAMPLER_HLE)
        if (sy) {
            std::memcpy(dst, work_.data() + size_t(y * scale_) * width() * 3, size_t(width()) * 3);
            continue;
        }
#endif
        for (unsigned x = 0; x < width_; ++x)
            for (unsigned sx = 0; sx < scale_; ++sx, dst += 3)
                std::memcpy(dst, native_row + x * 3, 3);
    }
    wrote_line(y);
}
void GbaHighResolutionFrame::latch() {
    if (written_lines_ != height_) { complete_ = false; return; }
    std::memcpy(latched_.data(), work_.data(), work_.size());
    complete_ = true; ++frames_;
}
} // namespace gba
