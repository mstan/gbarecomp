#include "gba_ppu.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

void store16(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
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

struct Fixture {
    std::array<uint8_t, 0x400> io{};
    std::array<uint8_t, 0x18000> vram{};
    std::array<uint8_t, 0x400> oam{};
    std::array<uint8_t, 0x400> pal{};
    std::array<uint8_t, gba::GbaPpu::kFramebufferBytes> rgb{};
    gba::GbaPpu ppu;
};

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

} // namespace

int main() {
    test_alpha_native_domain_and_green_precision();
    test_brightness_native_domain_and_green_precision();
    std::puts("ppu_smoke_tests: PASS");
    return 0;
}
