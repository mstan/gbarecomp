#include "presentation_layout.h"

#include <cstdio>
#include <cstdlib>

namespace {

void expect_layout(int drawable_width, int drawable_height,
                   int logical_width, int logical_height,
                   int x, int y, int width, int height, int integer_scale,
                   const char* label) {
    const auto got = gbarecomp::compute_presentation_layout(
        drawable_width, drawable_height, logical_width, logical_height);
    if (got.x != x || got.y != y || got.width != width ||
        got.height != height || got.integer_scale != integer_scale) {
        std::fprintf(stderr,
                     "%s: got {%d,%d %dx%d scale=%d}, expected "
                     "{%d,%d %dx%d scale=%d}\n",
                     label, got.x, got.y, got.width, got.height,
                     got.integer_scale, x, y, width, height, integer_scale);
        std::exit(1);
    }
    if (got.width != 0 &&
        static_cast<long long>(got.width) * logical_height !=
            static_cast<long long>(got.height) * logical_width) {
        std::fprintf(stderr, "%s: destination aspect ratio changed\n", label);
        std::exit(1);
    }
}

void expect_sharp_factor(int drawable_width, int drawable_height,
                         int logical_width, int logical_height,
                         int expected, const char* label) {
    const auto layout = gbarecomp::compute_presentation_layout(
        drawable_width, drawable_height, logical_width, logical_height);
    const int got = gbarecomp::compute_sharp_prescale_factor(
        layout, logical_width, logical_height);
    if (got != expected) {
        std::fprintf(stderr, "%s: got sharp factor %d, expected %d\n",
                     label, got, expected);
        std::exit(1);
    }
}

}  // namespace

int main() {
    const auto pixel_perfect = gbarecomp::compute_integer_presentation_layout(1000, 700, 240, 160);
    if (pixel_perfect.x != 20 || pixel_perfect.y != 30 || pixel_perfect.width != 960 ||
        pixel_perfect.height != 640 || pixel_perfect.integer_scale != 4) return 1;
    const auto adaptive_integer = gbarecomp::compute_integer_presentation_layout(1280, 720, 427, 240);
    if (adaptive_integer.width != 854 || adaptive_integer.height != 480 || adaptive_integer.integer_scale != 2) return 1;
    const auto small = gbarecomp::compute_integer_presentation_layout(120, 80, 240, 160);
    if (small.width != 120 || small.height != 80 || small.integer_scale != 0) return 1;
    expect_layout(720, 480, 240, 160,
                  0, 0, 720, 480, 3, "faithful exact 3x");
    expect_layout(1000, 700, 240, 160,
                  0, 17, 999, 666, 0, "faithful responsive fill");
    expect_layout(1152, 640, 288, 160,
                  0, 0, 1152, 640, 4, "widescreen exact 4x");
    expect_layout(1280, 720, 288, 160,
                  1, 5, 1278, 710, 0, "widescreen responsive fill");
    expect_layout(1000, 700, 288, 160,
                  0, 72, 999, 555, 0, "widescreen non-integer drawable");
    expect_layout(200, 150, 240, 160,
                  1, 9, 198, 132, 0, "faithful exact-ratio downscale");
    expect_layout(200, 120, 288, 160,
                  1, 5, 198, 110, 0, "widescreen exact-ratio downscale");
    expect_layout(0, 480, 240, 160,
                  0, 0, 0, 0, 0, "invalid drawable");
    expect_sharp_factor(720, 480, 240, 160,
                        0, "exact scale skips sharp pass");
    expect_sharp_factor(1000, 700, 240, 160,
                        4, "faithful fractional sharp scale");
    expect_sharp_factor(1280, 720, 288, 160,
                        4, "widescreen fractional sharp scale");
    expect_sharp_factor(200, 150, 240, 160,
                        0, "downscale skips sharp pass");
    std::puts("presentation_layout_tests: PASS");
    return 0;
}
