#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>

namespace gbarecomp {

// Destination rectangle for presenting a fixed logical framebuffer inside a
// resizable drawable. The logical pixels themselves are never resized here;
// this describes only the SDL presentation copy.
struct PresentationLayout {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int integer_scale = 0;  // >0 when width/height are whole-pixel multiples.
};

inline PresentationLayout compute_presentation_layout(int drawable_width,
                                                       int drawable_height,
                                                       int logical_width,
                                                       int logical_height) {
    if (drawable_width <= 0 || drawable_height <= 0 ||
        logical_width <= 0 || logical_height <= 0) {
        return {};
    }

    // Maximize the destination at the exact reduced logical aspect. Working in
    // whole ratio units avoids floating-point drift and one-axis stretch, while
    // still making every ordinary drag-resize visibly change the presentation.
    // Exact logical multiples naturally retain integer scaling.
    const int divisor = std::gcd(logical_width, logical_height);
    const int aspect_width = logical_width / divisor;
    const int aspect_height = logical_height / divisor;
    const int units = std::min(drawable_width / aspect_width,
                               drawable_height / aspect_height);
    if (units < 1) return {};
    const int width = aspect_width * units;
    const int height = aspect_height * units;
    const int integer_scale = units % divisor == 0 ? units / divisor : 0;

    return {
        (drawable_width - width) / 2,
        (drawable_height - height) / 2,
        width,
        height,
        integer_scale,
    };
}

// Adaptive geometry is rounded to logical pixels; requiring an exact reduced
// ratio can shrink coprime dimensions (240x427, for example) to integer scale.
// Fit mode instead rounds the contained destination to physical pixels.
inline PresentationLayout compute_adaptive_presentation_layout(int drawable_width,
                                                               int drawable_height,
                                                               int logical_width,
                                                               int logical_height) {
    if (drawable_width <= 0 || drawable_height <= 0 || logical_width <= 0 || logical_height <= 0)
        return {};
    int width = drawable_width, height = drawable_height;
    if (std::int64_t(drawable_width) * logical_height <= std::int64_t(drawable_height) * logical_width)
        height = std::max(1, static_cast<int>((std::int64_t(width) * logical_height + logical_width / 2) / logical_width));
    else
        width = std::max(1, static_cast<int>((std::int64_t(height) * logical_width + logical_height / 2) / logical_height));
    const int scale = width % logical_width == 0 && height % logical_height == 0 &&
                      width / logical_width == height / logical_height ? width / logical_width : 0;
    return {(drawable_width - width) / 2, (drawable_height - height) / 2, width, height, scale};
}

// Map a drawable-space point (physical pixels, origin at the drawable's top
// left) into the logical framebuffer shown by `layout`. Coordinates are
// continuous: logical pixel (3, 5) spans [3,4) x [5,6). Returns true when the
// point lies inside the presented image; the mapped coordinates are always
// written so callers can reason about points in the surrounding bars.
inline bool presentation_point_to_logical(const PresentationLayout& layout,
                                          int logical_width, int logical_height,
                                          float drawable_x, float drawable_y,
                                          float* logical_x, float* logical_y) {
    if (layout.width <= 0 || layout.height <= 0 ||
        logical_width <= 0 || logical_height <= 0) {
        if (logical_x) *logical_x = 0.0f;
        if (logical_y) *logical_y = 0.0f;
        return false;
    }
    const float lx = (drawable_x - static_cast<float>(layout.x)) *
                     static_cast<float>(logical_width) /
                     static_cast<float>(layout.width);
    const float ly = (drawable_y - static_cast<float>(layout.y)) *
                     static_cast<float>(logical_height) /
                     static_cast<float>(layout.height);
    if (logical_x) *logical_x = lx;
    if (logical_y) *logical_y = ly;
    return lx >= 0.0f && ly >= 0.0f &&
           lx < static_cast<float>(logical_width) &&
           ly < static_cast<float>(logical_height);
}

// Inverse of presentation_point_to_logical.
inline void logical_point_to_presentation(const PresentationLayout& layout,
                                          int logical_width, int logical_height,
                                          float logical_x, float logical_y,
                                          float* drawable_x, float* drawable_y) {
    if (layout.width <= 0 || layout.height <= 0 ||
        logical_width <= 0 || logical_height <= 0) {
        if (drawable_x) *drawable_x = 0.0f;
        if (drawable_y) *drawable_y = 0.0f;
        return;
    }
    if (drawable_x)
        *drawable_x = static_cast<float>(layout.x) +
                      logical_x * static_cast<float>(layout.width) /
                          static_cast<float>(logical_width);
    if (drawable_y)
        *drawable_y = static_cast<float>(layout.y) +
                      logical_y * static_cast<float>(layout.height) /
                          static_cast<float>(logical_height);
}

// Sharp fractional scaling first expands every logical pixel to the largest
// whole-number block that fits inside the final destination. A second,
// typically small linear stretch reaches the exact responsive layout. Exact
// integer layouts need no intermediate pass; downscales cannot use one.
inline int compute_sharp_prescale_factor(const PresentationLayout& layout,
                                         int logical_width,
                                         int logical_height) {
    if (layout.width <= 0 || layout.height <= 0 ||
        logical_width <= 0 || logical_height <= 0 ||
        layout.integer_scale > 0) {
        return 0;
    }
    const int factor = std::min(layout.width / logical_width,
                                layout.height / logical_height);
    return factor >= 2 ? factor : 0;
}

}  // namespace gbarecomp
