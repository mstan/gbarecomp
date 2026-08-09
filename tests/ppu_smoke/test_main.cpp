#include "gba_ppu.h"
#include "foreign_presentation_internal.h"
#include "snapshot.h"
#include "view_config.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace {

void store16(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
}

void store32(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
    dst[2] = static_cast<uint8_t>(value >> 16);
    dst[3] = static_cast<uint8_t>(value >> 24);
}

void expect_pixel(const uint8_t* actual,
                  uint8_t r,
                  uint8_t g,
                  uint8_t b,
                  const char* label) {
    if (actual[0] == r && actual[1] == g && actual[2] == b) return;
    std::fprintf(stderr,
                 "%s: expected RGB(%u,%u,%u), got RGB(%u,%u,%u)\n",
                 label, r, g, b, actual[0], actual[1], actual[2]);
    std::exit(1);
}

// Manual visual QA only: leave normal tests hermetic, but let CI/developers
// request the deterministic focus fixture as a portable screenshot artifact.
void dump_focus_screenshot_if_requested(const uint8_t* rgb) {
    const char* path = std::getenv("GBARECOMP_PPU_FOCUS_SCREENSHOT");
    if (!path || !*path) return;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << "P6\n240 160\n255\n";
    file.write(reinterpret_cast<const char*>(rgb),
               gba::GbaPpu::kFramebufferBytes);
}

struct Fixture {
    std::array<uint8_t, 0x400> io{};
    std::array<uint8_t, 0x18000> vram{};
    std::array<uint8_t, 0x400> oam{};
    std::array<uint8_t, 0x400> pal{};
    std::array<uint8_t, gba::GbaPpu::kFramebufferBytes> rgb{};
    gba::GbaPpu ppu;
};

void set_bg2_identity(Fixture& f) {
    store16(&f.io[0x20], 0x0100);  // PA = 1.0
    store16(&f.io[0x22], 0x0000);  // PB = 0.0
    store16(&f.io[0x24], 0x0000);  // PC = 0.0
    store16(&f.io[0x26], 0x0100);  // PD = 1.0
    store32(&f.io[0x28], 0);       // BG2X = 0.0
    store32(&f.io[0x2C], 0);       // BG2Y = 0.0
}

void disable_all_objects(Fixture& f) {
    for (std::size_t i = 0; i < 128; ++i) {
        store16(&f.oam[i * 8], 0x0200);
    }
}

int test_positive_obj_x(int raw_x, int* out_x) {
    if (raw_x >= 0x100 && raw_x < 288 && out_x) {
        *out_x = raw_x;
        return 1;
    }
    return 0;
}

int g_obj_attr_provider_calls = 0;
int test_obj_attr_x(int index, uint16_t attr0, uint16_t attr1,
                    uint16_t attr2, int* out_x) {
    ++g_obj_attr_provider_calls;
    if (index != 0 || attr0 != 0 || attr1 != 10 || attr2 != 0 || !out_x)
        return 0;
    *out_x = 260;
    return 1;
}

int g_margin_provider_action = gba::kWsTilemapReplace;

int test_margin_tilemap(int bg, int, int, uint16_t* out_entry) {
    if (bg != 0 || !out_entry) return 0;
    *out_entry = 0;
    return g_margin_provider_action;
}

int test_affine_filter(int bg, int) {
    return bg == 2;
}

int g_bg_x_provider_calls = 0;

int test_bg_x_provider(int bg, int output_x, int, int* out_hw_x) {
    ++g_bg_x_provider_calls;
    if (bg != 0) return 0;
    if (output_x == 0 && out_hw_x) {
        *out_hw_x = 0;
        return 1;
    }
    if (output_x == 24) return -1;
    return 0;
}

void test_alpha_native_domain_and_green_precision() {
    Fixture f;
    // Mode 0 BG0, 256-color tile 0 at character base 0, map at 0x800.
    const uint16_t dispcnt = 0x0100;
    store16(&f.io[0x08], 0x0180); // 256 colors, screen base block 1.
    f.vram[0] = 1;                // First pixel uses palette entry 1.
    store16(&f.vram[0x800], 0);   // Tile-map entry 0.

    // Top: RGB5(10,20,5), hidden green low bit set. Bottom: RGB5(20,5,25).
    store16(&f.pal[2], static_cast<uint16_t>(
        0x8000 | (5 << 10) | (20 << 5) | 10));
    store16(&f.pal[0], static_cast<uint16_t>(
        (25 << 10) | (5 << 5) | 20));
    store16(&f.io[0x50], 0x2041); // BG0 first, alpha, backdrop second.
    store16(&f.io[0x52], 0x100B); // EVA=11, EVB=16.

    f.ppu.render(f.rgb.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    // Native-domain result is RGB5(27,19,28), expanded after blending.
    expect_pixel(f.rgb.data(), 222, 156, 231, "alpha native-domain rounding");
}

void test_brightness_native_domain_and_green_precision() {
    Fixture f;
    const uint16_t source = static_cast<uint16_t>(
        0x8000 | (3 << 10) | (10 << 5) | 5);
    store16(&f.pal[0], source);
    store16(&f.io[0x54], 7);

    // Backdrop first target, brighten effect. Native result RGB5(16,19,15).
    store16(&f.io[0x50], 0x00A0);
    f.ppu.render(f.rgb.data(), 0, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 132, 156, 123, "brighten native-domain rounding");

    // Same source and coefficient, darken effect. Native result RGB5(3,6,2).
    store16(&f.io[0x50], 0x00E0);
    f.ppu.render(f.rgb.data(), 0, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 24, 49, 16, "darken native-domain rounding");
}

void test_extended_view_geometry_and_clamp() {
    gba::GbaPpu ppu;
    ppu.set_view_margins(24, 24, 0, 0);
    if (ppu.render_width() != 288 || ppu.render_height() != 160 ||
        ppu.view_extra_left() != 24 || ppu.view_extra_right() != 24) {
        std::fprintf(stderr, "extended-view 288x160 geometry mismatch\n");
        std::exit(1);
    }

    ppu.set_view_margins(72, 72, 0, 0);
    if (ppu.render_width() != 384 || ppu.view_extra_left() != 72 ||
        ppu.view_extra_right() != 72) {
        std::fprintf(stderr, "extended-view 384x160 geometry mismatch\n");
        std::exit(1);
    }

    ppu.set_view_margins(120, 120, 0, 0);
    if (ppu.render_width() != 480 ||
        ppu.render_width() != gba::GbaPpu::kMaxRenderWidth) {
        std::fprintf(stderr, "extended-view 480x160 capacity mismatch\n");
        std::exit(1);
    }

    ppu.set_view_margins(1000, 1000, 7, 9);
    if (ppu.render_width() != gba::GbaPpu::kMaxRenderWidth ||
        ppu.render_height() != gba::GbaPpu::kScreenHeight ||
        ppu.view_extra_top() != 0 || ppu.view_extra_bottom() != 0) {
        std::fprintf(stderr, "extended-view clamp mismatch\n");
        std::exit(1);
    }
}

void test_extended_view_capability_policy() {
    using gbarecomp::resolve_view_geometry;
    constexpr uint32_t kEngineMax = gba::GbaPpu::kMaxRenderWidth;

    auto g = resolve_view_geometry(288, 240, false, kEngineMax);
    if (g.width != 240 || g.extra_left != 0 || g.extra_right != 0) {
        std::fprintf(stderr, "unsupported extended view was not inert\n");
        std::exit(1);
    }
    g = resolve_view_geometry(288, 320, false, kEngineMax);
    if (g.width != 288 || g.extra_left != 24 || g.extra_right != 24) {
        std::fprintf(stderr, "opted-in 288x160 geometry mismatch\n");
        std::exit(1);
    }
    g = resolve_view_geometry(368, 320, false, kEngineMax);
    if (g.width != 320 || g.extra_left != 40 || g.extra_right != 40) {
        std::fprintf(stderr, "per-game maximum was not enforced\n");
        std::exit(1);
    }
    g = resolve_view_geometry(384, 480, false, kEngineMax);
    if (g.width != 384 || g.extra_left != 72 || g.extra_right != 72) {
        std::fprintf(stderr, "opted-in 384x160 geometry mismatch\n");
        std::exit(1);
    }
    g = resolve_view_geometry(480, 480, false, kEngineMax);
    if (g.width != 480 || g.extra_left != 120 || g.extra_right != 120) {
        std::fprintf(stderr, "opted-in 480x160 geometry mismatch\n");
        std::exit(1);
    }
    g = resolve_view_geometry(600, 600, false, kEngineMax);
    if (g.width != 480 || g.extra_left != 120 || g.extra_right != 120) {
        std::fprintf(stderr, "480x160 engine capacity was not enforced\n");
        std::exit(1);
    }
    g = resolve_view_geometry(288, 240, true, kEngineMax);
    if (g.width != 288) {
        std::fprintf(stderr, "development override did not bypass capability\n");
        std::exit(1);
    }
    g = resolve_view_geometry(285, 320, false, kEngineMax);
    if (g.width != 285 || g.extra_left != 22 || g.extra_right != 23) {
        std::fprintf(stderr, "odd extended-view split mismatch\n");
        std::exit(1);
    }

    const int legacy_extra[] = {0, 20, 22, 24, 40, 72, 120};
    const int expected_width[] = {240, 280, 284, 288, 320, 384, 480};
    for (std::size_t i = 0; i < std::size(legacy_extra); ++i) {
        int width = 0;
        if (!gbarecomp::legacy_extra_to_view_width(legacy_extra[i], &width) ||
            width != expected_width[i]) {
            std::fprintf(stderr, "legacy widescreen conversion mismatch\n");
            std::exit(1);
        }
    }
    int ignored = 0;
    if (gbarecomp::legacy_extra_to_view_width(-1, &ignored) ||
        gbarecomp::legacy_extra_to_view_width(
            std::numeric_limits<int>::max(), &ignored)) {
        std::fprintf(stderr, "legacy widescreen overflow was accepted\n");
        std::exit(1);
    }
}

void test_resize_driven_view_policy() {
    using gbarecomp::resize_driven_view_width;
    struct Case { int w; int h; uint32_t max; uint32_t expected; };
    const Case cases[] = {
        {720, 480, 480, 240}, {3440, 1440, 480, 382},
        {2560, 1080, 480, 379}, {1920, 1080, 320, 284},
        {800, 1200, 480, 240}, {10000, 1000, 480, 480},
        {0, 0, 480, 240},
    };
    for (const Case& c : cases) {
        if (resize_driven_view_width(c.w, c.h, c.max, 480) != c.expected) {
            std::fprintf(stderr, "resize-driven view policy mismatch\n");
            std::exit(1);
        }
    }
}

void test_extended_view_preserves_authentic_center() {
    Fixture f;
    const uint16_t dispcnt = 0x0100;  // Mode 0, BG0.
    store16(&f.io[0x08], 0x0180);     // 256 colors, screen block 1.
    store16(&f.io[0x10], 13);         // Non-tile-aligned horizontal scroll.
    store16(&f.io[0x12], 5);          // Non-tile-aligned vertical scroll.

    // Give every texel and palette entry a deterministic nontrivial value so
    // the comparison covers tile selection, scrolling, and RGB conversion.
    for (std::size_t i = 0; i < 64; ++i) f.vram[i] = static_cast<uint8_t>(i + 1);
    for (std::size_t i = 0; i < 32u * 32u; ++i)
        store16(&f.vram[0x800 + i * 2], static_cast<uint16_t>(i & 1u));
    for (unsigned i = 1; i < 256; ++i)
        store16(&f.pal[i * 2], static_cast<uint16_t>(
            (i & 31u) | (((i * 3u) & 31u) << 5) | (((i * 7u) & 31u) << 10)));

    std::vector<uint8_t> authentic(gba::GbaPpu::kFramebufferBytes, 0);
    f.ppu.render(authentic.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());

    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    const std::size_t authentic_stride = gba::GbaPpu::kScreenWidth * 3u;
    for (const uint32_t extra : {24u, 72u, 120u}) {
        f.ppu.set_view_margins(extra, extra, 0, 0);
        std::fill(wide.begin(), wide.end(), 0);
        f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                     f.oam.data(), f.pal.data());
        const std::size_t wide_stride = f.ppu.render_width() * 3u;
        for (uint32_t y = 0; y < gba::GbaPpu::kScreenHeight; ++y) {
            const uint8_t* got = wide.data() +
                y * wide_stride + extra * 3u;
            const uint8_t* expected = authentic.data() + y * authentic_stride;
            if (std::memcmp(got, expected, authentic_stride) != 0) {
                std::fprintf(stderr,
                             "extended-view %ux160 center differs from "
                             "authentic row %u\n",
                             f.ppu.render_width(), y);
                std::exit(1);
            }
        }
    }
}

void test_extended_bg_sample_remap_is_opt_in_and_native_inert() {
    Fixture f;
    const uint16_t dispcnt = 0x0100;  // Mode 0, BG0.
    store16(&f.io[0x08], 0x0180);     // 256 colors, screen block 1.
    std::fill_n(&f.vram[0], 64, 1);   // Tile 0: red.
    std::fill_n(&f.vram[64], 64, 2);  // Tile 1: blue.
    store16(&f.vram[0x800], 0);       // Authentic hardware X=0.
    store16(&f.vram[0x800 + 29 * 2], 1);  // Wrapped wide X=-24.
    store16(&f.pal[0], 0x03E0);       // Green backdrop.
    store16(&f.pal[2], 0x001F);       // Red tile 0.
    store16(&f.pal[4], 0x7C00);       // Blue tile 1.

    gba::g_ws_bg_x_provider = test_bg_x_provider;
    gba::g_ws_bg_x_provider_layers = 0xFu;
    g_bg_x_provider_calls = 0;
    f.ppu.render(f.rgb.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_bg_x_provider_calls != 0) {
        std::fprintf(stderr, "native renderer called wide BG remap provider\n");
        std::exit(1);
    }
    expect_pixel(f.rgb.data(), 255, 0, 0,
                 "native BG changed by wide remap provider");

    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_bg_x_provider_calls == 0) {
        std::fprintf(stderr, "wide renderer did not call BG remap provider\n");
        std::exit(1);
    }
    expect_pixel(wide.data(), 255, 0, 0,
                 "wide BG remap did not sample authentic X");
    expect_pixel(wide.data() + 24u * 3u, 0, 255, 0,
                 "wide BG suppress did not expose backdrop");

    g_bg_x_provider_calls = 0;
    gba::g_ws_bg_x_provider_layers = 0;
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_bg_x_provider_calls != 0) {
        std::fprintf(stderr, "wide renderer ignored BG provider layer mask\n");
        std::exit(1);
    }
    gba::g_ws_bg_x_provider_layers = 0xFu;
    gba::g_ws_bg_x_provider = nullptr;
}

void test_extended_view_snapshot_latch_policy() {
    constexpr std::size_t kPpuHeaderBytes = 3u * 4u + 2u + 8u + 1u;
    constexpr std::size_t kSnapshotBytes =
        kPpuHeaderBytes + gba::GbaPpu::kFramebufferBytes;

    // Native serialization remains the historical fixed header followed by the
    // contiguous 240x160 latch, byte for byte.
    gba::GbaPpu native;
    uint8_t* native_latch = const_cast<uint8_t*>(native.latched_framebuffer());
    for (std::size_t i = 0; i < gba::GbaPpu::kFramebufferBytes; ++i)
        native_latch[i] = static_cast<uint8_t>((i * 17u + 11u) & 0xFFu);
    native.mark_framebuffer_latched();
    gbarecomp::debug::SnapshotWriter native_writer;
    native.serialize(native_writer);
    std::vector<uint8_t> historical(kSnapshotBytes, 0);
    historical[kPpuHeaderBytes - 1u] = 1u;
    std::memcpy(historical.data() + kPpuHeaderBytes, native_latch,
                gba::GbaPpu::kFramebufferBytes);
    if (native_writer.buffer() != historical) {
        std::fprintf(stderr, "native PPU snapshot bytes changed\n");
        std::exit(1);
    }

    // A 480-wide latch must serialize the authentic center row-by-row, not the
    // first 240x160 bytes of its wider row-major allocation.
    gba::GbaPpu wide;
    wide.set_view_margins(120, 120, 0, 0);
    uint8_t* wide_latch = const_cast<uint8_t*>(wide.latched_framebuffer());
    for (uint32_t y = 0; y < wide.render_height(); ++y) {
        for (uint32_t x = 0; x < wide.render_width(); ++x) {
            for (uint32_t channel = 0; channel < 3; ++channel) {
                wide_latch[(static_cast<std::size_t>(y) * wide.render_width() + x) *
                               3u + channel] =
                    static_cast<uint8_t>((y * 7u + x * 3u + channel) & 0xFFu);
            }
        }
    }
    wide.mark_framebuffer_latched();
    gbarecomp::debug::SnapshotWriter wide_writer;
    wide.serialize(wide_writer);
    if (wide_writer.size() != kSnapshotBytes) {
        std::fprintf(stderr, "wide PPU snapshot layout size changed\n");
        std::exit(1);
    }
    const uint8_t* payload = wide_writer.buffer().data() + kPpuHeaderBytes;
    constexpr std::size_t kNativeStride = gba::GbaPpu::kScreenWidth * 3u;
    for (uint32_t y = 0; y < gba::GbaPpu::kScreenHeight; ++y) {
        const uint8_t* expected = wide_latch +
            (static_cast<std::size_t>(y) * wide.render_width() + 120u) * 3u;
        if (std::memcmp(payload + y * kNativeStride, expected,
                        kNativeStride) != 0) {
            std::fprintf(stderr, "wide snapshot center crop failed at row %u\n", y);
            std::exit(1);
        }
    }

    // Native loads retain the stored center latch. Wide loads consume the same
    // fixed payload but invalidate it because no serialized margin pixels exist.
    gba::GbaPpu native_loaded;
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 7;
    gba::g_ws_pillarbox_right = 9;
    gbarecomp::debug::SnapshotReader native_reader(
        wide_writer.buffer().data(), wide_writer.size());
    native_loaded.deserialize(native_reader);
    if (!native_reader.ok() || native_reader.remaining() != 0 ||
        !native_loaded.has_latched_framebuffer() ||
        std::memcmp(native_loaded.latched_framebuffer(), payload,
                    gba::GbaPpu::kFramebufferBytes) != 0) {
        std::fprintf(stderr, "wide-to-native snapshot latch restore failed\n");
        std::exit(1);
    }
    if (gba::g_ws_pillarbox != 0 || gba::g_ws_pillarbox_left != 7 ||
        gba::g_ws_pillarbox_right != 9) {
        std::fprintf(stderr, "native snapshot load changed margin policy\n");
        std::exit(1);
    }

    gba::GbaPpu wide_loaded;
    wide_loaded.set_view_margins(120, 120, 0, 0);
    gba::g_ws_authored_margin_layers = 0;
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 1;
    gba::g_ws_pillarbox_right = 1;
    gbarecomp::debug::SnapshotReader wide_reader(
        wide_writer.buffer().data(), wide_writer.size());
    wide_loaded.deserialize(wide_reader);
    if (!wide_reader.ok() || wide_reader.remaining() != 0 ||
        wide_loaded.has_latched_framebuffer() || gba::g_ws_pillarbox != 1 ||
        gba::g_ws_pillarbox_left != 0 || gba::g_ws_pillarbox_right != 0) {
        std::fprintf(stderr, "wide snapshot presentation latch was not invalidated\n");
        std::exit(1);
    }
    gba::g_ws_pillarbox = 0;

    // A self-sufficient game provider can explicitly authorize immediate
    // margin reconstruction from restored guest state. This must not weaken
    // the established default used by MMZ and the generic sidecar above.
    gba::GbaPpu authored_loaded;
    authored_loaded.set_view_margins(120, 120, 0, 0);
    gba::g_ws_authored_margin_layers = 1;
    gba::g_ws_pillarbox = 0;
    gbarecomp::debug::SnapshotReader authored_reader(
        wide_writer.buffer().data(), wide_writer.size());
    authored_loaded.deserialize(authored_reader);
    if (!authored_reader.ok() || authored_reader.remaining() != 0 ||
        authored_loaded.has_latched_framebuffer() || gba::g_ws_pillarbox != 0) {
        std::fprintf(stderr,
                     "authored margin provider was pillarboxed after restore\n");
        std::exit(1);
    }
    gba::g_ws_authored_margin_layers = 0;
}

void test_extended_view_obj_x_is_explicitly_opt_in() {
    Fixture f;
    for (std::size_t i = 0; i < 128; ++i)
        store16(&f.oam[i * 8], 0x0200);  // Disable every OBJ.
    store16(&f.oam[0], 0x0000);          // Enable OBJ 0 at Y=0.
    store16(&f.oam[2], 0x0100);          // Raw 9-bit X=256.
    store16(&f.oam[4], 0x0000);          // 4bpp tile 0, OBJ palette 0.
    f.vram[0x10000] = 0x11;              // First two texels use color 1.
    store16(&f.pal[0x202], 0x001F);      // OBJ color 1 = red.

    gba::g_ws_obj_x_provider = test_positive_obj_x;
    f.ppu.render(f.rgb.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    // The faithful renderer must ignore the extended-view provider.
    expect_pixel(f.rgb.data(), 0, 0, 0, "faithful OBJ X remained signed");

    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    f.ppu.render(wide.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    const std::size_t extended_x = (256u + 24u) * 3u;
    expect_pixel(wide.data() + extended_x, 255, 0, 0,
                 "opted-in extended OBJ X");
    gba::g_ws_obj_x_provider = nullptr;
}

void test_obj_only_palette_backdrop_is_not_policy_black() {
    Fixture f;
    for (std::size_t i = 0; i < 128; ++i)
        store16(&f.oam[i * 8], 0x0200);  // Disable every OBJ.
    store16(&f.pal[0], 0x03E0);          // Backdrop = green.
    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);

    gba::g_ws_pillarbox = 1;
    gba::g_ws_pillarbox_left = 1;
    gba::g_ws_pillarbox_right = 1;
    f.ppu.render(wide.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 255, 0,
                 "OBJ-only left palette backdrop");
    expect_pixel(wide.data() + (287u * 3u), 0, 255, 0,
                 "OBJ-only right palette backdrop");

    // The exception is deliberately narrow: once a regular BG is enabled,
    // the normal fail-closed policy still protects unauthored margin samples.
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x1100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0,
                 "regular-BG margin remained policy black");
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 0;
    gba::g_ws_pillarbox_right = 0;
}

void test_extended_view_obj_attr_x_is_explicitly_opt_in() {
    Fixture f;
    for (std::size_t i = 0; i < 128; ++i)
        store16(&f.oam[i * 8], 0x0200);
    store16(&f.oam[0], 0x0000);
    store16(&f.oam[2], 10);
    store16(&f.oam[4], 0);
    f.vram[0x10000] = 0x11;
    store16(&f.pal[0x202], 0x001F);

    g_obj_attr_provider_calls = 0;
    gba::g_ws_obj_attr_x_provider = test_obj_attr_x;
    f.ppu.render(f.rgb.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_obj_attr_provider_calls != 0) {
        std::fprintf(stderr, "native renderer called OBJ attribute provider\n");
        std::exit(1);
    }
    expect_pixel(f.rgb.data() + 10u * 3u, 255, 0, 0,
                 "native OBJ moved by attribute provider");

    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    f.ppu.render(wide.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_obj_attr_provider_calls == 0) {
        std::fprintf(stderr, "wide renderer skipped OBJ attribute provider\n");
        std::exit(1);
    }
    expect_pixel(wide.data() + (24u + 260u) * 3u, 255, 0, 0,
                 "attribute-aware HUD OBJ placement");
    gba::g_ws_obj_attr_x_provider = nullptr;
}

void test_extended_view_extends_nearest_window_edge() {
    Fixture f;
    store16(&f.io[0x08], 0x0180);  // BG0 256-color, screen block 1.
    std::fill_n(f.vram.begin(), 64, static_cast<uint8_t>(1));
    store16(&f.vram[0x800], 0);
    store16(&f.pal[2], 0x001F);     // Red.
    store16(&f.io[0x40], 0x00F0);   // WIN0 X=[0,240).
    store16(&f.io[0x44], 0x00A0);   // WIN0 Y=[0,160).
    store16(&f.io[0x48], 0x0001);   // WIN0 enables BG0.
    store16(&f.io[0x4A], 0x0000);   // WINOUT disables everything.
    f.ppu.set_view_margins(24, 24, 0, 0);
    gba::g_ws_tilemap_provider = test_margin_tilemap;
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 255, 0, 0,
                 "left margin inherited visible window edge");
    expect_pixel(wide.data() + (287u * 3u), 255, 0, 0,
                 "right margin inherited visible window edge");

    // A game can explicitly retain a wrapped entry for an intentionally
    // tiled effect without weakening the default fail-closed margin policy.
    std::fill_n(f.vram.begin() + 64, 64, static_cast<uint8_t>(2));
    store16(&f.pal[4], 0x03E0);     // Green.
    store16(&f.vram[0x800 + 29 * 2], 1);  // hx=-24 wraps to tile column 29.
    g_margin_provider_action = gba::kWsTilemapKeepWrapped;
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 255, 0,
                 "provider did not retain authored wrapped overlay");
    g_margin_provider_action = gba::kWsTilemapReplace;

    // With both authentic edges outside a smaller iris, margins inherit the
    // masked edge instead of bypassing WINOUT.
    store16(&f.io[0x40], 0x32BE);   // WIN0 X=[50,190).
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0,
                 "left margin inherited masked iris edge");
    expect_pixel(wide.data() + (287u * 3u), 0, 0, 0,
                 "right margin inherited masked iris edge");

    // Inverse apertures can make the authentic edge visible while the center
    // is hidden. Extending that edge would leak scenery around a closed wipe.
    store16(&f.io[0x48], 0x0000);
    store16(&f.io[0x4A], 0x0001);
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0,
                 "inverse aperture failed margin closed");
    expect_pixel(wide.data() + (287u * 3u), 0, 0, 0,
                 "inverse aperture failed right margin closed");

    // A narrow guest mask between the edge and center probes is still a
    // non-uniform scanline. The former edge/center-only classifier missed it
    // and extended visible WINOUT into both margins.
    store16(&f.io[0x40], 0x1428);   // WIN0 X=[20,40), covers no edge or center.
    store16(&f.io[0x48], 0x0000);   // WIN0 disables BG0.
    store16(&f.io[0x4A], 0x0001);   // WINOUT enables BG0.
    store16(&f.pal[0], 0x03E0);      // Nonblack green backdrop.
    std::vector<uint8_t> authentic(gba::GbaPpu::kFramebufferBytes, 0);
    f.ppu.set_view_margins(0, 0, 0, 0);
    f.ppu.render(authentic.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    f.ppu.set_view_margins(24, 24, 0, 0);
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0,
                 "narrow off-center mask failed left margin closed");
    expect_pixel(wide.data() + (287u * 3u), 0, 0, 0,
                 "narrow off-center mask failed right margin closed");
    for (uint32_t row = 0; row < gba::GbaPpu::kScreenHeight; ++row) {
        const uint8_t* got = wide.data() +
            (row * f.ppu.render_width() + f.ppu.view_extra_left()) * 3u;
        const uint8_t* expected = authentic.data() +
            row * gba::GbaPpu::kScreenWidth * 3u;
        if (std::memcmp(got, expected,
                        gba::GbaPpu::kScreenWidth * 3u) != 0) {
            std::fprintf(stderr,
                         "narrow mask changed authentic center row %u\n", row);
            std::exit(1);
        }
    }

    // Minish's full-room buffers are independent of native 240px HUD/dialog
    // windows. Its separate opt-in may reconstruct only the regular-BG
    // margins while leaving the authentic center masked exactly as authored.
    store16(&f.io[0x40], 0x32BE);   // Non-uniform native window.
    store16(&f.io[0x48], 0x0000);   // Disable BG0 inside WIN0.
    store16(&f.io[0x4A], 0x0000);   // Disable BG0 in WINOUT too.
    gba::g_ws_authored_margin_layers = 1;
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 255, 0, 0,
                 "authored provider did not reconstruct left margin");
    expect_pixel(wide.data() + (287u * 3u), 255, 0, 0,
                 "authored provider did not reconstruct right margin");
    expect_pixel(wide.data() + ((24u + 100u) * 3u), 0, 255, 0,
                 "authored margin policy changed native window center");
    gba::g_ws_authored_margin_layers = 0;
    gba::g_ws_tilemap_provider = nullptr;
    g_margin_provider_action = gba::kWsTilemapReplace;
}

void test_bitmap_mode3_direct_color_and_affine_origin() {
    Fixture f;
    set_bg2_identity(f);
    store16(&f.pal[0], 0x7C00);  // Blue backdrop.

    store16(&f.vram[(0 * 240 + 0) * 2], 0x001F);      // Red.
    store16(&f.vram[(9 * 240 + 17) * 2], 0x03E0);     // Green.
    store16(&f.vram[(159 * 240 + 239) * 2], 0x7C00);  // Blue.

    f.ppu.render(f.rgb.data(), 0x0403, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(&f.rgb[(0 * 240 + 0) * 3], 255, 0, 0,
                 "mode 3 first direct-color pixel");
    expect_pixel(&f.rgb[(9 * 240 + 17) * 3], 0, 255, 0,
                 "mode 3 interior direct-color pixel");
    expect_pixel(&f.rgb[(159 * 240 + 239) * 3], 0, 0, 255,
                 "mode 3 last direct-color pixel");

    // Shift the affine origin by (17,9): screen pixel (0,0) must sample the
    // green texel above.
    store32(&f.io[0x28], 17u << 8);
    store32(&f.io[0x2C], 9u << 8);
    f.ppu.render(f.rgb.data(), 0x0403, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 0, 255, 0,
                 "mode 3 affine origin");
}

void test_bitmap_mode4_palette_transparency_and_page_flip() {
    Fixture f;
    set_bg2_identity(f);
    store16(&f.pal[0], 0x7C00);  // Blue backdrop / transparent index 0.
    store16(&f.pal[2], 0x001F);  // Index 1 red.
    store16(&f.pal[4], 0x03E0);  // Index 2 green.

    f.vram[0] = 0;
    f.vram[1] = 1;
    f.vram[0xA000] = 0;
    f.vram[0xA001] = 2;

    f.ppu.render(f.rgb.data(), 0x0404, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(&f.rgb[0], 0, 0, 255,
                 "mode 4 palette index 0 transparency");
    expect_pixel(&f.rgb[3], 255, 0, 0,
                 "mode 4 frame 0 palette lookup");

    f.ppu.render(f.rgb.data(), 0x0414, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(&f.rgb[0], 0, 0, 255,
                 "mode 4 frame 1 index 0 transparency");
    expect_pixel(&f.rgb[3], 0, 255, 0,
                 "mode 4 frame 1 page selection");
}

void test_bitmap_mode5_bounds_and_page_flip() {
    Fixture f;
    set_bg2_identity(f);
    store16(&f.pal[0], 0x7FFF);  // White backdrop.

    store16(&f.vram[(0 * 160 + 0) * 2], 0x001F);
    store16(&f.vram[(127 * 160 + 159) * 2], 0x03E0);
    store16(&f.vram[0xA000], 0x7C00);

    f.ppu.render(f.rgb.data(), 0x0405, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(&f.rgb[(0 * 240 + 0) * 3], 255, 0, 0,
                 "mode 5 frame 0 first pixel");
    expect_pixel(&f.rgb[(127 * 240 + 159) * 3], 0, 255, 0,
                 "mode 5 frame 0 last pixel");
    expect_pixel(&f.rgb[(0 * 240 + 160) * 3], 255, 255, 255,
                 "mode 5 right letterbox");
    expect_pixel(&f.rgb[(128 * 240 + 0) * 3], 255, 255, 255,
                 "mode 5 bottom letterbox");

    f.ppu.render(f.rgb.data(), 0x0415, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 0, 0, 255,
                 "mode 5 frame 1 page selection");
}

void test_bitmap_mode_obj_compositing_and_obj_window() {
    Fixture f;
    set_bg2_identity(f);
    disable_all_objects(f);
    store16(&f.io[0x0C], 1);      // BG2 priority 1.
    store16(&f.pal[0], 0x7C00);   // Blue backdrop.
    store16(&f.pal[2], 0x03E0);   // BG index 1 green.
    store16(&f.pal[0x202], 0x001F);  // OBJ index 1 red.
    std::fill_n(f.vram.begin(), 240 * 160, static_cast<uint8_t>(1));
    std::fill_n(f.vram.begin() + 0x14000, 32, static_cast<uint8_t>(0x11));

    // Normal 8x8, 4bpp OBJ at (0,0), hardware tile 512, priority 0.
    // Bitmap modes do not rebase tile 0 to 0x14000; tiles 0..511 are ignored.
    store16(&f.oam[0], 0x0000);
    store16(&f.oam[2], 0x0000);
    store16(&f.oam[4], 0x0200);
    f.ppu.render(f.rgb.data(), 0x1404, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 255, 0, 0,
                 "mode 4 OBJ over bitmap BG");
    expect_pixel(&f.rgb[8 * 3], 0, 255, 0,
                 "mode 4 bitmap BG outside OBJ");

    // A populated lower OBJ tile remains unavailable in bitmap modes.
    std::fill_n(f.vram.begin() + 0x10000, 32, static_cast<uint8_t>(0x11));
    store16(&f.oam[4], 0x0000);
    f.ppu.render(f.rgb.data(), 0x1404, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 0, 255, 0,
                 "mode 4 illegal lower OBJ tile was not ignored");

    // Turn that OBJ into an OBJ-window stencil. Outside enables BG2; inside
    // disables every layer, revealing the backdrop.
    store16(&f.oam[0], 0x0800);
    store16(&f.oam[4], 0x0200);
    store16(&f.io[0x4A], 0x0004);
    f.ppu.render(f.rgb.data(), 0x8404, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 0, 0, 255,
                 "mode 4 OBJ-window mask");
    expect_pixel(&f.rgb[8 * 3], 0, 255, 0,
                 "mode 4 outside OBJ-window");
}

void test_bitmap_mode_wide_center_and_margins() {
    Fixture f;
    set_bg2_identity(f);
    store16(&f.pal[0], 0x7C00);  // Blue backdrop.
    store16(&f.pal[2], 0x001F);  // Red.
    store16(&f.pal[4], 0x03E0);  // Green.
    f.vram[0] = 1;
    f.vram[239] = 2;
    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);

    f.ppu.render(wide.data(), 0x0404, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 255,
                 "mode 4 wide left margin");
    expect_pixel(&wide[24 * 3], 255, 0, 0,
                 "mode 4 wide authentic first pixel");
    expect_pixel(&wide[(24 + 239) * 3], 0, 255, 0,
                 "mode 4 wide authentic last pixel");
    expect_pixel(&wide[(24 + 240) * 3], 0, 0, 255,
                 "mode 4 wide right margin");
}

void test_trusted_foreign_background_keeps_guest_objects_and_resets() {
    Fixture f;
    disable_all_objects(f);
    std::array<uint16_t, gba::GbaPpu::kScreenWidth * gba::GbaPpu::kScreenHeight>
        foreign{};
    const GbaForeignObjFocusTransform focus{
        GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION,
        0, 0, 8, 8,
        8, 8,
        GBA_FOREIGN_OBJ_FOCUS_PRESERVE_UNFOCUSED,
        0, 0,
        0, 0,
    };
    foreign.fill(0x001F);  // Red foreign terrain.

    // Stock backdrop is blue, which gives a byte-for-byte inactive baseline.
    store16(&f.pal[0], 0x7C00);
    f.ppu.render(f.rgb.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    const auto stock = f.rgb;

    // A normal priority-3 OBJ remains above the foreign background.  Its tile
    // is green in OBJ palette bank zero and appears at the top-left.
    std::fill_n(f.vram.begin() + 0x10000, 32, static_cast<uint8_t>(0x11));
    store16(&f.pal[0x202], 0x03E0);
    store16(&f.oam[0], 0x0000);
    store16(&f.oam[2], 0x0000);
    store16(&f.oam[4], 0x0C00);  // Tile 0, OBJ priority 3.
    gba::foreign_presentation_internal::set_background(foreign.data());
    f.ppu.render(f.rgb.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 0, 255, 0,
                 "foreign background keeps guest OBJ");
    expect_pixel(&f.rgb[8 * 3], 255, 0, 0,
                 "foreign background replaces guest backdrop");

    // The normal semitransparent-OBJ path still blends against the foreign
    // BG2 replacement when the game's BLDCNT names BG2 as second target.
    store16(&f.oam[0], 0x0400);  // OBJ mode 1 = semitransparent.
    store16(&f.io[0x50], 0x0440);  // Alpha, BG2 second target.
    store16(&f.io[0x52], 0x0808);  // Equal OBJ/foreign blend.
    f.ppu.render(f.rgb.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 132, 123, 0,
                 "foreign background is semitransparent OBJ target");

    // A trusted foreign frame is opaque to stale native windows. The one-pixel
    // WIN0 disables guest BG2, but must not reveal screen-fixed guest room
    // tiles over a later foreign room; OBJ composition remains covered above.
    store16(&f.io[0x40], 0x0001);  // WIN0 x=[0,1)
    store16(&f.io[0x44], 0x00A0);  // WIN0 y=[0,160)
    store16(&f.io[0x48], 0x0010);  // WIN0: OBJ only, no BG2.
    store16(&f.io[0x4A], 0x0004);  // Outside: BG2.
    disable_all_objects(f);
    f.ppu.render(f.rgb.data(), 0x3000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 255, 0, 0,
                 "foreign background ignores stale native BG2 window mask");

    // Reset and clearing the provider both return to the literal stock path;
    // neither stale host presentation nor a callback survives reset.
    gbarecomp::debug::SnapshotWriter writer;
    f.ppu.serialize(writer);
    gbarecomp::debug::SnapshotReader reader(writer.buffer().data(),
                                              writer.buffer().size());
    gba::foreign_presentation_internal::set_obj_focus(&focus);
    f.ppu.deserialize(reader);
    if (gba::foreign_presentation_internal::background() != nullptr ||
        gba::foreign_presentation_internal::obj_focus() != nullptr) {
        std::fprintf(stderr, "foreign presentation survived PPU savestate load\n");
        std::exit(1);
    }
    gba::foreign_presentation_internal::set_background(foreign.data());
    gba::foreign_presentation_internal::set_obj_focus(&focus);
    f.ppu.reset();
    if (gba::foreign_presentation_internal::background() != nullptr ||
        gba::foreign_presentation_internal::obj_focus() != nullptr) {
        std::fprintf(stderr, "foreign presentation survived PPU reset\n");
        std::exit(1);
    }
    disable_all_objects(f);
    f.ppu.render(f.rgb.data(), 0x0000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (std::memcmp(f.rgb.data(), stock.data(), stock.size()) != 0) {
        std::fprintf(stderr, "inactive foreign background changed stock PPU output\n");
        std::exit(1);
    }
}

void test_trusted_foreign_obj_focus_transforms_only_matching_objects() {
    // Normal OBJ origin matching, unrelated OBJ preservation, and inactive
    // byte identity use the same native composition path.
    Fixture f;
    disable_all_objects(f);
    store16(&f.pal[0], 0x7C00);       // Blue backdrop.
    store16(&f.pal[0x202], 0x001F);   // OBJ palette 1 red.
    store16(&f.pal[0x204], 0x03E0);   // OBJ palette 2 green.
    std::fill_n(f.vram.begin() + 0x10000, 32, static_cast<uint8_t>(0x11));
    std::fill_n(f.vram.begin() + 0x10020, 32, static_cast<uint8_t>(0x22));
    store16(&f.oam[0], 20);
    store16(&f.oam[2], 20);
    store16(&f.oam[4], 0);
    store16(&f.oam[8], 20);
    store16(&f.oam[10], 80);
    store16(&f.oam[12], 1);
    // 32x32 OBJ whose origin is outside the focus rectangle but whose center
    // is inside it, exercising the Link-body association fallback.
    store16(&f.oam[16], 4);
    store16(&f.oam[18], 0x8000 | 4);
    store16(&f.oam[20], 1);
    f.ppu.render(f.rgb.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    const auto stock = f.rgb;

    const GbaForeignObjFocusTransform focus{
        GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION,
        20, 20, 30, 28,
        8, 8,
        GBA_FOREIGN_OBJ_FOCUS_PRESERVE_UNFOCUSED,
        0, 0,
        0, 0,
    };
    gba::foreign_presentation_internal::set_obj_focus(&focus);
    f.ppu.render(f.rgb.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    dump_focus_screenshot_if_requested(f.rgb.data());
    expect_pixel(&f.rgb[(20 * 240 + 20) * 3], 0, 0, 255,
                 "focused normal OBJ leaves source location");
    expect_pixel(&f.rgb[(28 * 240 + 30) * 3], 255, 0, 0,
                 "focused normal OBJ reaches destination");
    expect_pixel(&f.rgb[(4 * 240 + 4) * 3], 0, 0, 255,
                 "focused center-match OBJ leaves source location");
    expect_pixel(&f.rgb[(12 * 240 + 14) * 3], 0, 255, 0,
                 "focused center-match OBJ reaches destination");
    for (int y = 20; y < 28; ++y) {
        if (std::memcmp(&f.rgb[(y * 240 + 80) * 3],
                        &stock[(y * 240 + 80) * 3], 8 * 3) != 0) {
            std::fprintf(stderr, "unfocused OBJ changed during focus transform\n");
            std::exit(1);
        }
    }
    gba::foreign_presentation_internal::set_obj_focus(nullptr);
    f.ppu.render(f.rgb.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (std::memcmp(f.rgb.data(), stock.data(), stock.size()) != 0) {
        std::fprintf(stderr, "inactive OBJ focus changed stock PPU output\n");
        std::exit(1);
    }

    // A source-mapped player body can require origin-only matching.  The
    // 32x32 green room prop has its center at the Link source feet but an
    // origin outside the bounded player-body rectangle, so it must stay put
    // while the red player OBJ moves.  This models a doorway behind Link
    // without hiding the prop or unrelated guest OBJ.
    Fixture strict;
    disable_all_objects(strict);
    store16(&strict.pal[0], 0x7C00);
    store16(&strict.pal[0x202], 0x001F);
    store16(&strict.pal[0x204], 0x03E0);
    std::fill_n(strict.vram.begin() + 0x10000, 32,
                static_cast<uint8_t>(0x11));
    std::fill_n(strict.vram.begin() + 0x10020, 32,
                static_cast<uint8_t>(0x22));
    std::fill_n(strict.vram.begin() + 0x10040, 32,
                static_cast<uint8_t>(0x22));
    store16(&strict.oam[0], 20);
    store16(&strict.oam[2], 20);
    store16(&strict.oam[4], 0);
    store16(&strict.oam[8], 4);
    store16(&strict.oam[10], 0x8000 | 4);  // 32x32 prop centered at (20,20).
    store16(&strict.oam[12], 1);
    store16(&strict.oam[16], 0x4000 | 28); // 16x8 source-player shadow.
    store16(&strict.oam[18], 22);
    store16(&strict.oam[20], 2);
    const GbaForeignObjFocusTransform origin_only{
        GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION,
        20, 20, 60, 60,
        8, 8,
        GBA_FOREIGN_OBJ_FOCUS_PRESERVE_UNFOCUSED |
            GBA_FOREIGN_OBJ_FOCUS_ORIGIN_ONLY |
        GBA_FOREIGN_OBJ_FOCUS_SOURCE_TILE_RANGE |
            GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_LARGE_NEARBY,
        0, 1,
        2, 1,
    };
    gba::foreign_presentation_internal::set_obj_focus(&origin_only);
    strict.ppu.render(strict.rgb.data(), 0x1000, strict.io.data(),
                      strict.vram.data(), strict.oam.data(), strict.pal.data());
    expect_pixel(&strict.rgb[(60 * 240 + 60) * 3], 255, 0, 0,
                 "origin-only focus moves source player origin");
    expect_pixel(&strict.rgb[(68 * 240 + 62) * 3], 0, 255, 0,
                 "source focus moves the bounded player shadow allocation");
    expect_pixel(&strict.rgb[(4 * 240 + 4) * 3], 0, 0, 255,
                 "bounded source focus suppresses centered room prop");
    expect_pixel(&strict.rgb[(44 * 240 + 44) * 3], 0, 0, 255,
                 "origin-only focus does not translate centered room prop");
    gba::foreign_presentation_internal::set_obj_focus(nullptr);

    // The PPU independently validates the immutable descriptor. A corrupted
    // pointer that bypassed publication fails closed to the stock result.
    GbaForeignObjFocusTransform malformed_focus = focus;
    malformed_focus.flags = 0x80000000u;
    gba::foreign_presentation_internal::set_obj_focus(&malformed_focus);
    f.ppu.render(f.rgb.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (std::memcmp(f.rgb.data(), stock.data(), stock.size()) != 0) {
        std::fprintf(stderr, "malformed OBJ focus did not fail closed\n");
        std::exit(1);
    }
    gba::foreign_presentation_internal::set_obj_focus(nullptr);

    // Affine and semi-transparent modes use the same translated decoded
    // coordinates before their established PPU paths, not a special blit.
    Fixture affine;
    disable_all_objects(affine);
    store16(&affine.pal[0], 0x7C00);
    store16(&affine.pal[0x202], 0x001F);
    std::fill_n(affine.vram.begin() + 0x10000, 32,
                static_cast<uint8_t>(0x11));
    // Matrix group zero is stored in the interleaved entries 0..3. OBJ 4 uses
    // it while those entries remain disabled.
    store16(&affine.oam[6], 0x0100);
    store16(&affine.oam[14], 0);
    store16(&affine.oam[22], 0);
    store16(&affine.oam[30], 0x0100);
    store16(&affine.oam[32], 0x0100 | 40);  // Affine OBJ at (20,40).
    store16(&affine.oam[34], 20);
    store16(&affine.oam[36], 0);
    const GbaForeignObjFocusTransform affine_focus{
        GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION,
        20, 40, 30, 50,
        8, 8,
        0,
        0, 0,
        0, 0,
    };
    gba::foreign_presentation_internal::set_obj_focus(&affine_focus);
    affine.ppu.render(affine.rgb.data(), 0x1000, affine.io.data(),
                      affine.vram.data(), affine.oam.data(), affine.pal.data());
    expect_pixel(&affine.rgb[(50 * 240 + 30) * 3], 255, 0, 0,
                 "focused affine OBJ reaches destination");

    Fixture translucent;
    disable_all_objects(translucent);
    store16(&translucent.pal[0], 0x7C00);
    store16(&translucent.pal[0x202], 0x001F);
    std::fill_n(translucent.vram.begin() + 0x10000, 32,
                static_cast<uint8_t>(0x11));
    store16(&translucent.oam[0], 0x0400 | 60);  // Mode 1 at (20,60).
    store16(&translucent.oam[2], 20);
    store16(&translucent.oam[4], 0);
    store16(&translucent.io[0x50], 0x2000);  // Backdrop is second target.
    store16(&translucent.io[0x52], 0x0808);
    const GbaForeignObjFocusTransform translucent_focus{
        GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION,
        20, 60, 30, 70,
        8, 8,
        0,
        0, 0,
        0, 0,
    };
    gba::foreign_presentation_internal::set_obj_focus(&translucent_focus);
    translucent.ppu.render(translucent.rgb.data(), 0x1000,
                            translucent.io.data(), translucent.vram.data(),
                            translucent.oam.data(), translucent.pal.data());
    expect_pixel(&translucent.rgb[(70 * 240 + 30) * 3], 132, 0, 132,
                 "focused semitransparent OBJ blends at destination");

    // Signed OAM wrap happens before focus; a clipped (-4,-4) OBJ can become
    // visible at (0,0) without mutating the wrapped guest OAM value.
    Fixture wrapped;
    disable_all_objects(wrapped);
    store16(&wrapped.pal[0], 0x7C00);
    store16(&wrapped.pal[0x202], 0x001F);
    std::fill_n(wrapped.vram.begin() + 0x10000, 32,
                static_cast<uint8_t>(0x11));
    store16(&wrapped.oam[0], 0x00FC);
    store16(&wrapped.oam[2], 0x01FC);
    store16(&wrapped.oam[4], 0);
    const GbaForeignObjFocusTransform wrapped_focus{
        GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION,
        0, 0, 4, 4,
        8, 8,
        0,
        0, 0,
        0, 0,
    };
    gba::foreign_presentation_internal::set_obj_focus(&wrapped_focus);
    wrapped.ppu.render(wrapped.rgb.data(), 0x1000, wrapped.io.data(),
                       wrapped.vram.data(), wrapped.oam.data(), wrapped.pal.data());
    expect_pixel(&wrapped.rgb[(4 * 240 + 4) * 3], 255, 0, 0,
                 "focused wrapped OBJ reaches unclipped destination");
    gba::foreign_presentation_internal::set_obj_focus(nullptr);
}

void test_affine_reference_reload_overrides_scanline_accumulation() {
    Fixture f;
    disable_all_objects(f);

    // Mode 1 BG2, 128x128 affine map at block 0, 256-color tiles at
    // character block 1. Tile 0 is red; its right-hand neighbor is green.
    const uint16_t dispcnt = 0x0401;
    store16(&f.io[0x0C], 0x0084);
    f.vram[0] = 0;
    f.vram[1] = 1;
    std::fill_n(f.vram.begin() + 0x4000, 64, static_cast<uint8_t>(1));
    std::fill_n(f.vram.begin() + 0x4040, 64, static_cast<uint8_t>(2));
    store16(&f.pal[2], 0x001F);
    store16(&f.pal[4], 0x03E0);

    store16(&f.io[0x20], 0x0100);  // PA = 1 pixel per output pixel.
    store16(&f.io[0x22], 0x0800);  // PB = 8 pixels per scanline.
    store16(&f.io[0x24], 0x0000);
    store16(&f.io[0x26], 0x0000);
    store32(&f.io[0x28], 0);
    store32(&f.io[0x2C], 0);

    f.ppu.render_scanline(0, dispcnt, f.io.data(), f.vram.data(),
                          f.oam.data(), f.pal.data());

    // HBlank DMA writes the same zero BG2X again. The write itself reloads the
    // hidden affine reference, so line 1 must still begin at red tile 0. The
    // old ref + y*PB shortcut incorrectly began at green tile 1.
    f.ppu.note_affine_reference_write(2, false);
    f.ppu.render_scanline(1, dispcnt, f.io.data(), f.vram.data(),
                          f.oam.data(), f.pal.data());

    // With no second reload, the internal reference advances by PB and line 2
    // correctly begins at green tile 1.
    f.ppu.render_scanline(2, dispcnt, f.io.data(), f.vram.data(),
                          f.oam.data(), f.pal.data());
    f.ppu.mark_framebuffer_latched();
    const uint8_t* frame = f.ppu.latched_framebuffer();
    expect_pixel(frame + (0 * 240) * 3, 255, 0, 0,
                 "affine line 0 reference");
    expect_pixel(frame + (1 * 240) * 3, 255, 0, 0,
                 "affine HBlank reference reload");
    expect_pixel(frame + (2 * 240) * 3, 0, 255, 0,
                 "affine internal reference accumulation");
}

void test_wide_affine_filter_is_selective_and_bilinear() {
    Fixture f;
    disable_all_objects(f);

    // Mode 1 BG2, 128x128 affine map at block 0, 256-color tile data at
    // character block 1. Sample halfway between a red and green texel.
    const uint16_t dispcnt = 0x0401;
    store16(&f.io[0x0C], 0x0084);
    f.vram[0] = 0;
    f.vram[0x4000] = 1;
    f.vram[0x4001] = 2;
    f.vram[0x4008] = 1;
    f.vram[0x4009] = 2;
    store16(&f.pal[2], 0x001F);
    store16(&f.pal[4], 0x03E0);
    set_bg2_identity(f);
    store32(&f.io[0x28], 0x80);

    constexpr uint32_t margin = 24;
    f.ppu.set_view_margins(margin, margin, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);

    gba::g_ws_affine_filter_enabled = 0;
    gba::g_ws_affine_filter_provider = test_affine_filter;
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(&wide[margin * 3], 255, 0, 0,
                 "wide affine nearest baseline");

    gba::g_ws_affine_filter_enabled = 1;
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(&wide[margin * 3], 132, 132, 0,
                 "wide affine bilinear sample");

    gba::g_ws_affine_filter_provider = nullptr;
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(&wide[margin * 3], 255, 0, 0,
                 "wide affine provider opt-in");
    gba::g_ws_affine_filter_enabled = 0;
}

// The latched frame is a VBlank snapshot: scanlines of the NEXT frame being
// composited must never show through latched_framebuffer(). (Regression test
// for the Minish Cap walk "warble": render_scanline used to write directly
// into the latched buffer, so any consumer reading it after the guest ran
// into the next frame's visible lines saw a horizontal one-frame tear.)
void test_latched_frame_immune_to_in_progress_scanlines() {
    Fixture f;
    f.ppu.reset();
    const uint16_t dispcnt = 0;  // mode 0, no layers -> backdrop everywhere

    store16(&f.pal[0], 0x001F);  // frame A backdrop: red
    for (uint32_t y = 0; y < gba::GbaPpu::kScreenHeight; ++y)
        f.ppu.render_scanline(y, dispcnt, f.io.data(), f.vram.data(),
                              f.oam.data(), f.pal.data());
    f.ppu.mark_framebuffer_latched();
    if (!f.ppu.has_latched_framebuffer()) {
        std::fprintf(stderr, "latch immunity: frame A did not latch\n");
        std::exit(1);
    }
    std::array<uint8_t, gba::GbaPpu::kFramebufferBytes> frame_a{};
    std::memcpy(frame_a.data(), f.ppu.latched_framebuffer(), frame_a.size());

    // Frame B starts compositing (guest ran past VBlank into visible lines).
    store16(&f.pal[0], 0x03E0);  // frame B backdrop: green
    for (uint32_t y = 0; y < gba::GbaPpu::kScreenHeight / 2; ++y)
        f.ppu.render_scanline(y, dispcnt, f.io.data(), f.vram.data(),
                              f.oam.data(), f.pal.data());
    if (std::memcmp(f.ppu.latched_framebuffer(), frame_a.data(),
                    frame_a.size()) != 0) {
        std::fprintf(stderr,
                     "latch immunity: in-progress frame B scanlines leaked "
                     "into the latched frame A snapshot\n");
        std::exit(1);
    }

    // Completing frame B and latching publishes it.
    for (uint32_t y = gba::GbaPpu::kScreenHeight / 2;
         y < gba::GbaPpu::kScreenHeight; ++y)
        f.ppu.render_scanline(y, dispcnt, f.io.data(), f.vram.data(),
                              f.oam.data(), f.pal.data());
    f.ppu.mark_framebuffer_latched();
    if (std::memcmp(f.ppu.latched_framebuffer(), frame_a.data(),
                    frame_a.size()) == 0) {
        std::fprintf(stderr,
                     "latch immunity: completed frame B failed to publish\n");
        std::exit(1);
    }
    expect_pixel(f.ppu.latched_framebuffer(), 0, 255, 0,
                 "latch immunity: frame B backdrop");
}

} // namespace

int main() {
    test_alpha_native_domain_and_green_precision();
    test_brightness_native_domain_and_green_precision();
    test_extended_view_geometry_and_clamp();
    test_extended_view_capability_policy();
    test_resize_driven_view_policy();
    test_extended_view_preserves_authentic_center();
    test_extended_bg_sample_remap_is_opt_in_and_native_inert();
    test_extended_view_snapshot_latch_policy();
    test_obj_only_palette_backdrop_is_not_policy_black();
    test_extended_view_obj_x_is_explicitly_opt_in();
    test_extended_view_obj_attr_x_is_explicitly_opt_in();
    test_extended_view_extends_nearest_window_edge();
    test_bitmap_mode3_direct_color_and_affine_origin();
    test_bitmap_mode4_palette_transparency_and_page_flip();
    test_bitmap_mode5_bounds_and_page_flip();
    test_bitmap_mode_obj_compositing_and_obj_window();
    test_bitmap_mode_wide_center_and_margins();
    test_trusted_foreign_background_keeps_guest_objects_and_resets();
    test_trusted_foreign_obj_focus_transforms_only_matching_objects();
    test_affine_reference_reload_overrides_scanline_accumulation();
    test_wide_affine_filter_is_selective_and_bilinear();
    test_latched_frame_immune_to_in_progress_scanlines();
    std::puts("ppu_smoke_tests: PASS");
    return 0;
}
