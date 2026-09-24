#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace gbarecomp {

struct ViewGeometry {
    std::uint32_t width = 240;
    std::uint32_t extra_left = 0;
    std::uint32_t extra_right = 0;
    std::uint32_t height = 160;
    std::uint32_t extra_top = 0;
    std::uint32_t extra_bottom = 0;
};

inline bool legacy_extra_to_view_width(int extra_per_side, int* width) {
    if (!width || extra_per_side < 0 ||
        extra_per_side > (std::numeric_limits<int>::max() - 240) / 2) {
        return false;
    }
    *width = 240 + 2 * extra_per_side;
    return true;
}

// Pure policy resolver kept separate from the renderer so capability and
// faithful-default behavior can be unit-tested without launching a game.
inline ViewGeometry resolve_view_geometry(int requested_width,
                                          std::uint32_t game_max_width,
                                          bool development_override,
                                          std::uint32_t engine_max_width) {
    constexpr std::uint32_t kNativeWidth = 240;
    const std::uint32_t engine_max = std::max(kNativeWidth, engine_max_width);
    const std::uint32_t opted_in_max =
        std::clamp(game_max_width, kNativeWidth, engine_max);
    const std::uint32_t allowed_max =
        development_override ? engine_max : opted_in_max;
    const std::uint32_t requested = requested_width < 240
        ? kNativeWidth
        : static_cast<std::uint32_t>(requested_width);
    const std::uint32_t width = std::min(requested, allowed_max);
    const std::uint32_t extra = width - kNativeWidth;
    return {width, extra / 2u, extra - extra / 2u};
}

// Convert a host drawable aspect ratio into a logical GBA view width while
// keeping the authentic 160-line height. Narrower-than-native windows never
// crop the game; wider windows reveal more horizontal content up to game_max.
inline std::uint32_t resize_driven_view_width(int drawable_width,
                                              int drawable_height,
                                              std::uint32_t game_max_width,
                                              std::uint32_t engine_max_width) {
    constexpr std::uint32_t kNativeWidth = 240;
    constexpr std::uint32_t kNativeHeight = 160;
    const std::uint32_t maximum = std::clamp(
        game_max_width, kNativeWidth,
        std::max(kNativeWidth, engine_max_width));
    if (drawable_width <= 0 || drawable_height <= 0) return kNativeWidth;

    // Round to the nearest logical pixel. Use 64-bit arithmetic so very large
    // desktop dimensions cannot overflow before the clamp.
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(drawable_width) * kNativeHeight;
    const std::uint64_t rounded =
        (scaled + static_cast<std::uint64_t>(drawable_height) / 2u) /
        static_cast<std::uint64_t>(drawable_height);
    return static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>(rounded, kNativeWidth, maximum));
}

// Density-driven geometry (touch devices): the logical pixel keeps a physical
// size (`mm_per_logical_px`, scaled by the player's zoom) so a phone shows
// roughly the native short axis while a tablet reveals more world on BOTH
// axes, instead of the same world with bigger pixels. The view always fills
// the drawable at its aspect; it never shrinks below native 240x160 on
// either axis, and the scale grows past the physical target when the game's
// (or engine's) maximum logical size would otherwise be exceeded.
inline ViewGeometry density_driven_view_geometry(int drawable_width, int drawable_height,
                                                 float drawable_px_per_mm,
                                                 float mm_per_logical_px,
                                                 float zoom,
                                                 std::uint32_t game_max_width,
                                                 std::uint32_t game_max_height,
                                                 std::uint32_t engine_max_width,
                                                 std::uint32_t engine_max_height) {
    ViewGeometry result;
    if (drawable_width <= 0 || drawable_height <= 0) return result;
    const double max_w = static_cast<double>(std::clamp(
        game_max_width, 240u, std::max(240u, engine_max_width)));
    const double max_h = static_cast<double>(std::clamp(
        game_max_height, 160u, std::max(160u, engine_max_height)));
    const double dw = drawable_width, dh = drawable_height;
    const double target = std::max(0.25, static_cast<double>(drawable_px_per_mm)) *
                          std::max(0.05, static_cast<double>(mm_per_logical_px)) *
                          std::max(0.25, static_cast<double>(zoom));
    // Drawable pixels per logical pixel: at least the physical target, large
    // enough that the maxima hold, and small enough that neither axis drops
    // below native (a tiny window then keeps native size and letterboxes).
    double scale = std::max({target, dw / max_w, dh / max_h});
    scale = std::min(scale, std::min(dw / 240.0, dh / 160.0));
    if (scale <= 0.0) return result;
    auto clamp_axis = [](double v, double lo, double hi) {
        return static_cast<std::uint32_t>(std::clamp(std::floor(v + 0.5), lo, hi));
    };
    result.width = clamp_axis(dw / scale, 240.0, max_w);
    result.height = clamp_axis(dh / scale, 160.0, max_h);
    const std::uint32_t extra_w = result.width - 240u;
    const std::uint32_t extra_h = result.height - 160u;
    result.extra_left = extra_w / 2u;
    result.extra_right = extra_w - result.extra_left;
    result.extra_top = extra_h / 2u;
    result.extra_bottom = extra_h - result.extra_top;
    return result;
}

inline ViewGeometry resize_driven_view_geometry(int drawable_width, int drawable_height,
                                                std::uint32_t game_max_width,
                                                std::uint32_t game_max_height,
                                                std::uint32_t engine_max_width,
                                                std::uint32_t engine_max_height) {
    const auto width = resize_driven_view_width(drawable_width, drawable_height,
                                                game_max_width, engine_max_width);
    auto result = resolve_view_geometry(width, game_max_width, false, engine_max_width);
    if (drawable_width > 0 && drawable_height > 0 && width == 240) {
        const auto maximum = std::clamp(game_max_height, 160u, std::max(160u, engine_max_height));
        const auto height = (std::uint64_t(drawable_height) * 240 + drawable_width / 2) / drawable_width;
        result.height = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(height, 160, maximum));
        result.extra_top = (result.height - 160) / 2;
        result.extra_bottom = result.height - 160 - result.extra_top;
    }
    return result;
}

}  // namespace gbarecomp
