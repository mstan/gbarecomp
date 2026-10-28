#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace gba {

// Title authorization is read-only and evaluated at the real raster boundary.
using HighResolutionAffineProvider = bool (*)(int bg, int screen_y);

// Downstream presentation only. Native layers retain their authored pixels;
// approved affine BGs are sampled again at fractional output coordinates and
// inserted into the same priority/window/blend stack. No guest state is owned
// here. The caller must not serialize this object or use its output for reads.
class GbaHighResolutionFrame {
public:
    static constexpr unsigned kMaxScale = 16;
    static const char* sampler_implementation();
    explicit GbaHighResolutionFrame(unsigned scale);
    unsigned scale() const { return scale_; }
    void configure(unsigned width, unsigned height);
    void invalidate();
    void begin_frame();
    bool begin_line(int y, unsigned ox, unsigned oy, uint16_t dispcnt,
                    const uint8_t* io, const uint8_t* vram, const uint8_t* pal,
                    HighResolutionAffineProvider provider, bool filter);
    void window(unsigned x, uint8_t control);
    void submit(unsigned x, uint16_t color, int key, uint8_t layer,
                bool target1, bool target2);
    void black_column(unsigned x) { black_[x] = true; }
    void resolve();
    void upscale_line(unsigned y, const uint8_t* native_row);
    void latch();
    const uint8_t* pixels() const;
    uint8_t* native_scratch() { return scratch_.data(); }
    unsigned width() const { return width_ * scale_; }
    unsigned height() const { return height_ * scale_; }
    bool complete() const { return complete_; }
    uint64_t frames() const { return frames_; }
    uint64_t affine_lines() const { return affine_lines_; }
    uint64_t samples() const { return samples_; }
    uint64_t captured_lines() const { return captured_lines_; }
    uint64_t cpu_materializations() const { return cpu_materializations_; }

    // Downstream immutable draw inputs. Capturing these replaces the HD CPU
    // raster; a consumer may render them without reading live guest memory.
    // Keep native simulation and the portable renderer under their own contract.
    struct Pixel {
        uint16_t color = 0;
        int key = 0x7fffffff;
        uint8_t layer = 5;
        bool target1 = false, target2 = false, valid = false;
    };
    struct Affine {
        bool active = false;
        uint16_t cnt = 0;
        unsigned cache = 0;
        int y = -2;
        int64_t x = 0, v = 0, pa = 0, pc = 0;
        int64_t dx = 0, dv = 0, dpa = 0, dpc = 0;
    };
    struct IndexedSurface {
        unsigned size = 0;
        std::vector<uint8_t> texels;
    };
    struct LineJob {
        enum Kind { Missing, AffineLine, UpscaleLine } kind = Missing;
        unsigned ox = 0;
        uint16_t bldcnt = 0, alpha = 0;
        unsigned brightness = 0;
        bool filter = false;
        std::array<Affine, 2> affine{};
        std::array<std::shared_ptr<const IndexedSurface>, 2> surfaces;
        std::array<uint16_t, 256> palette{};
        std::vector<Pixel> top, second;
        std::vector<uint8_t> windows, black, native_rgb;
    };
    struct DrawFrame {
        unsigned width = 0, height = 0, scale = 0;
        std::vector<LineJob> lines;
    };
    // Internal backend selection, not a game enhancement toggle. The caller
    // selects only at a clean presentation transition and retains CPU fallback.
    void set_deferred_rendering(bool enabled);
    bool deferred_rendering() const { return deferred_; }
    std::shared_ptr<const DrawFrame> draw_frame() const { return complete_ ? latched_jobs_ : nullptr; }
    static std::vector<uint8_t> render_jobs(const DrawFrame& frame);

private:
    // Presentation-owned indexed surfaces. Palette colors are refreshed at each
    // published row, so HBlank palette writes never require a surface rebuild.
    struct Surface {
        unsigned layout = ~0u, size = 0;
        std::array<uint8_t, 16384> chars{};
        std::vector<uint8_t> map, texels;
        std::shared_ptr<const IndexedSurface> published;
        void update(uint16_t cnt, const uint8_t* vram);
        std::shared_ptr<const IndexedSurface> publish();
    };
    static void insert(Pixel& top, Pixel& second, const Pixel& candidate);
    bool sample(const Affine& bg, int64_t u, int64_t v, uint16_t& color) const;
    void write_color(uint8_t* dst, const Pixel& top, const Pixel& second) const;
    void wrote_line(unsigned y);
    void resolve_prepared();
    void prepare_surfaces_palette();
    unsigned scale_, width_ = 0, height_ = 0, ox_ = 0, row_ = 0;
    uint16_t bldcnt_ = 0, alpha_ = 0;
    unsigned brightness_ = 0, selected_ = 0;
    bool filter_ = false, complete_ = false;
    const uint8_t* vram_ = nullptr;
    const uint8_t* pal_ = nullptr;
    std::array<Affine, 2> previous_{}, affine_{};
    std::array<Surface, 2> surfaces_;
    std::array<const IndexedSurface*, 2> borrowed_surfaces_{};
    std::array<uint16_t, 256> palette_{};
    std::array<uint64_t, 256> filter_palette_{};
    std::vector<Pixel> top_, second_;
    std::vector<uint8_t> windows_, black_, written_, work_, latched_, scratch_;
    unsigned written_lines_ = 0;
    uint64_t frames_ = 0, affine_lines_ = 0, samples_ = 0;
    bool deferred_ = false;
    std::shared_ptr<DrawFrame> pending_jobs_;
    std::shared_ptr<const DrawFrame> latched_jobs_;
    mutable std::vector<uint8_t> materialized_;
    uint64_t captured_lines_ = 0;
    mutable uint64_t cpu_materializations_ = 0;
};

} // namespace gba
