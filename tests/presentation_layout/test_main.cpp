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

}  // namespace

int main() {
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
    std::puts("presentation_layout_tests: PASS");
    return 0;
}
