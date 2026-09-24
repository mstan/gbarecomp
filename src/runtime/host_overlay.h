// host_overlay.h — host-drawn presentation layer for game input policies.
//
// Drawn every present AFTER the game image and BEFORE the runtime settings
// menu. It never touches guest memory or the guest framebuffer: it is purely
// host chrome (touch trails, destination markers, large native buttons), so
// the faithful framebuffer, savestates and frame hashes are unaffected.
//
// Coordinates are either logical view pixels (OverlaySpace::View, the same
// space TouchEvent::view_x/y use) or physical drawable pixels.

#pragma once

#include <cstddef>
#include <cstdint>

#include "presentation_layout.h"

namespace gbarecomp {

enum class OverlaySpace : std::uint8_t { View = 0, Drawable = 1 };

enum class OverlayAlign : std::uint8_t { Left = 0, Center = 1, Right = 2 };

struct OverlayColor {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
};

struct OverlayPoint {
    float x = 0.0f, y = 0.0f;
};

class HostOverlay {
public:
    virtual ~HostOverlay() = default;

    // Geometry of this present.
    virtual int drawable_width() const = 0;
    virtual int drawable_height() const = 0;
    virtual PresentationLayout game_rect() const = 0;
    virtual int view_width() const = 0;
    virtual int view_height() const = 0;
    virtual float drawable_px_per_mm() const = 0;
    struct Insets { int left = 0, top = 0, right = 0, bottom = 0; };
    virtual Insets safe_insets() const = 0;
    virtual std::uint32_t host_ms() const = 0;

    virtual void to_drawable(float x, float y, OverlaySpace space,
                             float* dx, float* dy) const = 0;

    // Primitives. Thickness/radius/size are in the given space's units.
    virtual void line(float x0, float y0, float x1, float y1, float thickness,
                      OverlayColor color, OverlaySpace space) = 0;
    virtual void polyline(const OverlayPoint* points, std::size_t count,
                          float thickness, OverlayColor color,
                          OverlaySpace space) = 0;
    virtual void fill_rect(float x, float y, float w, float h, float rounding,
                           OverlayColor color, OverlaySpace space) = 0;
    virtual void stroke_rect(float x, float y, float w, float h, float rounding,
                             float thickness, OverlayColor color,
                             OverlaySpace space) = 0;
    virtual void fill_circle(float cx, float cy, float radius, OverlayColor color,
                             OverlaySpace space) = 0;
    virtual void stroke_circle(float cx, float cy, float radius, float thickness,
                               OverlayColor color, OverlaySpace space) = 0;
    // Partial ring from `start_turns` to `end_turns` (0..1, clockwise from
    // 12 o'clock) — progress indicators such as a long-press fill.
    virtual void arc(float cx, float cy, float radius, float thickness,
                     float start_turns, float end_turns, OverlayColor color,
                     OverlaySpace space) = 0;

    // Text is available only when the build includes the runtime UI font
    // stack. `size` is the glyph height in the given space.
    virtual bool text_supported() const = 0;
    virtual void text(float x, float y, float size, OverlayColor color,
                      const char* utf8, OverlayAlign align,
                      OverlaySpace space) = 0;
    virtual float text_width(const char* utf8, float size,
                             OverlaySpace space) const = 0;
};

}  // namespace gbarecomp
