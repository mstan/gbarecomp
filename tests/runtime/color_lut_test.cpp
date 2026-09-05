#include "color_lut.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

std::array<uint8_t, 32768u * 3u> make_hardware_expanded_rgb() {
    std::array<uint8_t, 32768u * 3u> out{};
    for (uint32_t px = 0; px < 32768u; ++px) {
        const uint8_t r = static_cast<uint8_t>(px & 31u);
        const uint8_t g = static_cast<uint8_t>((px >> 5) & 31u);
        const uint8_t b = static_cast<uint8_t>((px >> 10) & 31u);
        out[px * 3u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
        out[px * 3u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
        out[px * 3u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
    }
    return out;
}

bool same_bytes(const std::array<uint8_t, 32768u * 3u>& a,
                const std::array<uint8_t, 32768u * 3u>& b) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

}  // namespace

int main() {
    gbarecomp::runtime::ColorSettings raw{};
    raw.screen = gbarecomp::runtime::ScreenKind::Raw;
    gbarecomp::runtime::ColorLut lut(raw);
    if (!lut.is_passthrough()) {
        std::printf("FAIL: raw screen should be passthrough\n");
        return 1;
    }

    const auto src = make_hardware_expanded_rgb();
    std::array<uint8_t, 32768u * 3u> dst{};
    lut.map_rgb888(src.data(), dst.data(), 32768, 1);
    if (!same_bytes(src, dst)) {
        std::printf("FAIL: raw LUT changed hardware-expanded RGB\n");
        return 1;
    }

    dst = src;
    lut.map_rgb888(dst.data(), dst.data(), 32768, 1);
    if (!same_bytes(src, dst)) {
        std::printf("FAIL: raw LUT changed in-place hardware-expanded RGB\n");
        return 1;
    }

    std::printf("color_lut_tests: PASS\n");
    return 0;
}
