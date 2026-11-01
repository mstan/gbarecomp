#include "gba_high_resolution.h"
#include "gba_color.h"
#include "gba_render_profile.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gba {
namespace {
// Callers validate the logical dimensions and scale before requesting pixels.
// Form the product before narrowing so 32-bit hosts reject an oversized
// optional surface instead of wrapping its allocation size.
size_t rgb888_size(unsigned width, unsigned height, unsigned scale) {
    const uint64_t bytes = uint64_t(width) * height * scale * scale * 3;
    if (bytes > std::vector<uint8_t>().max_size())
        throw std::length_error("high-resolution RGB surface exceeds host allocation limits");
    return size_t(bytes);
}
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (unsigned(p[1]) << 8)); }
int64_t ref28(const uint8_t* p) {
    uint32_t n = uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16);
    return int32_t(n << 4) >> 4;
}
int64_t delta28(int64_t n) { return int32_t(uint32_t(n) << 4) >> 4; }
inline bool sample_indexed(const uint8_t* texels, unsigned size, uint16_t cnt,
    bool filter, const std::array<uint16_t,256>& palette,
    const std::array<uint64_t,256>& filter_palette, int64_t u, int64_t v,
    uint16_t& color) {
    int64_t x = u >> 8, y = v >> 8;
    const bool wrap = cnt & 0x2000;
    if (wrap) { x &= size - 1; y &= size - 1; }
    else if (x < 0 || y < 0 || x >= size || y >= size) return false;
    const unsigned c00 = texels[size_t(y) * size + size_t(x)];
    if (!c00) return false;
    color = palette[c00];
    if (!filter) return true;
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
    const uint64_t sum = filter_palette[c00] * ((256 - fx) * (256 - fy)) +
                         filter_palette[c10] * (fx * (256 - fy)) +
                         filter_palette[c01] * ((256 - fx) * fy) +
                         filter_palette[c11] * (fx * fy) + round;
    color = uint16_t(((sum >> 16) & 31) | (((sum >> 37) & 31) << 5) | (((sum >> 58) & 31) << 10));
    return true;
}
}
const char* GbaHighResolutionFrame::sampler_implementation() {
#if defined(GBARECOMP_HD_SAMPLER_HLE)
    return "HLE (cached indexed affine surface)";
#else
    return "LLE (direct tiled affine sampling)";
#endif
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
    published.reset();
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
}
std::shared_ptr<const GbaHighResolutionFrame::IndexedSurface>
GbaHighResolutionFrame::Surface::publish() {
    if (!published) published = std::make_shared<const IndexedSurface>(IndexedSurface{size, texels});
    return published;
}
GbaHighResolutionFrame::GbaHighResolutionFrame(unsigned scale) : scale_(scale) {
    if (scale < 2 || scale > kMaxScale)
        throw std::invalid_argument("high-resolution frame scale must be from 2 through 128");
}
void GbaHighResolutionFrame::configure(unsigned width, unsigned height) {
    if (width == width_ && height == height_) return;
    if (width < 240 || width > 1280 || height < 160 || height > 864)
        throw std::invalid_argument("invalid high-resolution presentation dimensions");
    width_ = width; height_ = height;
    top_.resize(width); second_.resize(width); windows_.resize(width); black_.resize(width);
    written_.resize(height);
    if (!deferred_) {
        work_.resize(rgb888_size(width, height, scale_));
        latched_.resize(work_.size());
    }
    scratch_.resize(size_t(width) * height * 3);
    invalidate();
}
void GbaHighResolutionFrame::invalidate() {
    complete_ = false;
    latched_jobs_.reset(); materialized_.clear();
    for (auto& surface : surfaces_) surface.layout = ~0u;
    begin_frame();
}
void GbaHighResolutionFrame::begin_frame() {
    previous_ = {};
    std::fill(written_.begin(), written_.end(), 0);
    written_lines_ = 0;
    if (deferred_) {
        pending_jobs_ = std::make_shared<DrawFrame>();
        pending_jobs_->width = width_; pending_jobs_->height = height_;
        pending_jobs_->scale = scale_; pending_jobs_->lines.resize(height_);
    } else pending_jobs_.reset();
}
bool GbaHighResolutionFrame::begin_line(int y, unsigned ox, unsigned oy,
    uint16_t dispcnt, const uint8_t* io, const uint8_t* vram, const uint8_t* pal,
    HighResolutionAffineProvider provider, bool filter) {
    GBA_RENDER_PHASE_BEGIN(profile, HdInput);
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
                                    uint16_t& color, bool filter) const {
    const unsigned size = 128u << (a.cnt >> 14);
#if defined(GBARECOMP_HD_SAMPLER_HLE)
    const auto* texels = borrowed_surfaces_[a.cache]
        ? borrowed_surfaces_[a.cache]->texels.data() : surfaces_[a.cache].texels.data();
    return sample_indexed(texels, size, a.cnt, filter, palette_, filter_palette_, u, v, color);
#else
    if (borrowed_surfaces_[a.cache])
        return sample_indexed(borrowed_surfaces_[a.cache]->texels.data(), size,
            a.cnt, filter, palette_, filter_palette_, u, v, color);
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
    if (!filter) return true;
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
void GbaHighResolutionFrame::prepare_surfaces_palette() {
    for (const auto& a : affine_) if (a.active) surfaces_[a.cache].update(a.cnt, vram_);
    for (unsigned i = 0; i < palette_.size(); ++i) {
        const uint16_t c = palette_[i] = u16(pal_ + i * 2);
        filter_palette_[i] = (c & 31) | (uint64_t((c >> 5) & 31) << 21) | (uint64_t((c >> 10) & 31) << 42);
    }
}
void GbaHighResolutionFrame::resolve() {
    GBA_RENDER_PHASE_BEGIN(profile, HdPrepare);
#if defined(GBARECOMP_HD_SAMPLER_HLE)
    prepare_surfaces_palette();
#else
    if (deferred_) prepare_surfaces_palette();
#endif
    if (deferred_) {
        if (!pending_jobs_) throw std::logic_error("begin_frame required before deferred raster");
        auto& job = pending_jobs_->lines.at(row_);
        job.kind = LineJob::AffineLine; job.ox = ox_;
        job.bldcnt = bldcnt_; job.alpha = alpha_; job.brightness = brightness_;
        job.filter = filter_; job.affine = affine_; job.palette = palette_;
        job.top = top_; job.second = second_; job.windows = windows_; job.black = black_;
        for (unsigned i = 0; i < 2; ++i)
            job.surfaces[i] = affine_[i].active ? surfaces_[i].publish() : nullptr;
        ++captured_lines_;
        wrote_line(row_);
        return;
    }
    GBA_RENDER_PHASE_MARK(profile, HdRaster);
    resolve_prepared();
}
void GbaHighResolutionFrame::resolve_prepared() {
    for (unsigned sy = 0; sy < scale_; ++sy) {
        struct Row {
            int64_t u = 0, v = 0, pa = 0, pc = 0;
            std::array<int64_t, kMaxScale> du, dv;
        };
        std::array<Row, 2> rows; // Active rows initialize every consumed subpixel offset.
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
#if defined(GBARECOMP_HD_SAMPLER_HLE)
            // Native HUD/OBJ pixels cover a whole output block. If no affine
            // layer can reach the winner (or its required alpha source),
            // resolve once and expand the block rather than sampling hidden
            // texels and rebuilding the same candidate stack per subpixel.
            const Pixel& base = top_[x];
            const unsigned effect = (bldcnt_ >> 6) & 3u;
            bool fixed = !((effect == 1 || base.layer == 4) && base.target1);
            for (unsigned i = 0; fixed && i < 2; ++i) {
                const auto& a = affine_[i];
                if (a.active && (windows_[x] & (1u << (i + 2))) &&
                    int((a.cnt & 3) * 256 + 130 + i) < base.key) fixed = false;
            }
            if (black_[x]) {
                std::memset(dst, 0, scale_ * 3);
                dst += scale_ * 3;
            } else if (fixed) {
                uint8_t rgb[3];
                write_color(rgb, base, second_[x]);
                for (unsigned sx = 0; sx < scale_; ++sx, dst += 3) std::memcpy(dst,rgb,3);
            } else
#endif
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
                    if (!sample(a, r.u + r.du[sx], r.v + r.dv[sx], color, filter_)) continue;
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
bool GbaHighResolutionFrame::try_render_native_line(uint8_t* dst,uint16_t dispcnt) {
    const unsigned mode=dispcnt&7;
    const unsigned native_affine=((dispcnt>>8)&15)&(mode==1 ? 4u : mode==2 ? 12u : 0u);
    if (!dst || !deferred_ || width_!=240 || height_!=160 || ox_!=0 ||
        !native_affine || (selected_&native_affine)!=native_affine ||
        !pending_jobs_ || row_>=pending_jobs_->lines.size() ||
        pending_jobs_->lines[row_].kind!=LineJob::AffineLine) return false;
    // Native positions have neither the presentation filter nor subscanline
    // interpolation. Reuse the prepared windows, non-affine candidates,
    // palettes and indexed surfaces; do not run a second compositor setup.
    std::array<int64_t,2> u{},v{};
    for (unsigned i=0;i<2;++i) {u[i]=affine_[i].x;v[i]=affine_[i].v;}
    for (unsigned x=0;x<width_;++x,dst+=3) {
        if (black_[x]) {dst[0]=dst[1]=dst[2]=0;}
        else {
            Pixel top=top_[x],second=second_[x];
            for (unsigned i=0;i<2;++i) {
                const auto& a=affine_[i]; const unsigned layer=i+2;
                if (!a.active || !(windows_[x]&(1u<<layer))) continue;
                uint16_t color;
                if (!sample(a,u[i],v[i],color,false)) continue;
                insert(top,second,{color,int((a.cnt&3)*256+128+layer),uint8_t(layer),
                    bool((windows_[x]&0x20)&&(bldcnt_&(1u<<layer))),
                    bool(bldcnt_&(0x100u<<layer)),true});
            }
            write_color(dst,top,second);
        }
        for(unsigned i=0;i<2;++i) {u[i]+=affine_[i].pa;v[i]+=affine_[i].pc;}
    }
    ++canonical_shared_lines_;
    return true;
}
void GbaHighResolutionFrame::upscale_line(unsigned y, const uint8_t* native_row) {
    GBA_RENDER_PHASE_BEGIN(profile, HdUpscale);
    if (deferred_) {
        if (!pending_jobs_) throw std::logic_error("begin_frame required before deferred raster");
        auto& job = pending_jobs_->lines.at(y);
        job.kind = LineJob::UpscaleLine;
        job.native_rgb.assign(native_row, native_row + width_ * 3);
        ++captured_lines_;
        wrote_line(y);
        return;
    }
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
    GBA_RENDER_PHASE_BEGIN(profile, HdLatch);
    if (written_lines_ != height_) { complete_ = false; return; }
    if (deferred_) {
        if (!pending_jobs_) return;
        latched_jobs_ = std::move(pending_jobs_);
        materialized_.clear();
    } else std::memcpy(latched_.data(), work_.data(), work_.size());
    complete_ = true; ++frames_;
}
void GbaHighResolutionFrame::set_deferred_rendering(bool enabled) {
    if (deferred_ == enabled) return;
    deferred_ = enabled;
    if (enabled) {
        std::vector<uint8_t>().swap(work_);
        std::vector<uint8_t>().swap(latched_);
    } else {
        work_.resize(rgb888_size(width_, height_, scale_));
        latched_.resize(work_.size());
    }
    invalidate();
}
const uint8_t* GbaHighResolutionFrame::pixels() const {
    if (!complete_) return nullptr;
    if (!deferred_) return latched_.data();
    if (!latched_jobs_) return nullptr;
    if (materialized_.empty()) {
        materialized_ = render_jobs(*latched_jobs_);
        ++cpu_materializations_;
    }
    return materialized_.data();
}
std::vector<uint8_t> GbaHighResolutionFrame::render_jobs(const DrawFrame& frame) {
    if (frame.width < 240 || frame.width > 1280 || frame.height < 160 || frame.height > 864 ||
        frame.scale < 2 || frame.scale > kMaxScale || frame.lines.size() != frame.height)
        throw std::invalid_argument("invalid HD draw frame");
    GbaHighResolutionFrame renderer(frame.scale);
    renderer.width_ = frame.width; renderer.height_ = frame.height;
    renderer.work_.resize(rgb888_size(frame.width, frame.height, frame.scale));
    renderer.written_.resize(frame.height);
    for (unsigned y = 0; y < frame.height; ++y) {
        const auto& job = frame.lines[y];
        if (job.kind == LineJob::UpscaleLine) {
            if (job.native_rgb.size() != size_t(frame.width) * 3)
                throw std::invalid_argument("invalid HD upscale row");
            renderer.upscale_line(y, job.native_rgb.data());
            continue;
        }
        if (job.kind != LineJob::AffineLine || job.top.size() != frame.width ||
            job.second.size() != frame.width || job.windows.size() != frame.width ||
            job.black.size() != frame.width)
            throw std::invalid_argument("incomplete HD draw row");
        renderer.row_ = y; renderer.ox_ = job.ox;
        renderer.bldcnt_ = job.bldcnt; renderer.alpha_ = job.alpha;
        renderer.brightness_ = job.brightness; renderer.filter_ = job.filter;
        renderer.affine_ = job.affine; renderer.palette_ = job.palette;
        renderer.top_ = job.top; renderer.second_ = job.second;
        renderer.windows_ = job.windows; renderer.black_ = job.black;
        for (unsigned i = 0; i < 256; ++i) {
            const uint16_t c = job.palette[i];
            renderer.filter_palette_[i] = (c & 31) | (uint64_t((c >> 5) & 31) << 21) |
                                          (uint64_t((c >> 10) & 31) << 42);
        }
        for (unsigned i = 0; i < 2; ++i) {
            const auto& surface = job.surfaces[i];
            if (job.affine[i].active && (job.affine[i].cache != i || !surface || surface->size != (128u << (job.affine[i].cnt >> 14)) ||
                surface->texels.size() != size_t(surface->size) * surface->size))
                throw std::invalid_argument("invalid HD indexed surface");
            renderer.borrowed_surfaces_[i] = surface.get();
        }
        renderer.resolve_prepared();
    }
    return std::move(renderer.work_);
}
} // namespace gba
