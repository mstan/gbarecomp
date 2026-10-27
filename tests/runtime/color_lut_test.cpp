#include "color_lut.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

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

    // Compare packed upload against the established RGB888 output for every
    // hardware color, all screen models and both native SDL channel orders.
    // The second input includes non-hardware low bits from filtered HD pixels:
    // raw must keep them; graded profiles must retain their existing 5-bit lookup.
    using namespace gbarecomp::runtime;
    std::vector<uint32_t> packed(32768);
    for (const auto screen : {ScreenKind::Raw, ScreenKind::Unlit, ScreenKind::Frontlit,
                              ScreenKind::Backlit, ScreenKind::Classic}) {
        for (const auto target : {DisplayTarget::Srgb, DisplayTarget::DisplayP3}) {
            ColorSettings settings{};settings.screen=screen;settings.target=target;
            ColorLut colors(settings);
            for (int filtered = 0; filtered < 2; ++filtered) {
                auto input=src;
                if (filtered) {
                    for (std::size_t i=0;i<input.size();++i)
                        input[i]=uint8_t((input[i]&0xf8u) | ((i*13u+5u)&7u));
                }
                colors.map_rgb888(input.data(),dst.data(),32768,1);
                for (const bool red_low : {false,true}) {
                    colors.map_packed8888(input.data(),packed.data(),32768,1,red_low);
                    for (std::size_t i=0;i<packed.size();++i) {
                        const uint32_t r=dst[i*3],g=dst[i*3+1],b=dst[i*3+2];
                        const uint32_t expected=0xff000000u|(g<<8)|(red_low ? r|(b<<16) : (r<<16)|b);
                        if (packed[i]!=expected) {
                            std::printf("FAIL: packed upload color/channel/alpha mismatch at %zu\n",i);return 1;
                        }
                    }
                }
            }
            colors.map_packed8888(nullptr,nullptr,0,1,false);
        }
    }

    std::printf("color_lut_tests: PASS\n");
    return 0;
}
