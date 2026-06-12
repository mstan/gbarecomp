// color_lut.h — present-time screen-color simulation (BGR555 → RGBA8 LUT).
//
// PRESENT-TIME ONLY. This never touches the emulation, the framebuffer the
// PPU produces, or the differential-verify path — diff_frame / oracle
// comparisons stay defined on the raw RGB888 the PPU renders. The LUT is a
// 32768-entry table applied to a COPY of the frame at SDL-upload time, and
// it defaults to Raw (exact passthrough), so default behavior and every
// hashed/verified frame are byte-identical unless a screen model is opted in
// via GBARECOMP_SCREEN={raw,unlit,frontlit,backlit,classic}.
//
// The math is first-principles CIE colorimetry (xyY→XYZ, primaries→matrix,
// Bradford adaptation, sRGB OETF) over published colorimeter measurements;
// it is not a ported shader.
//
// ── Attribution ───────────────────────────────────────────────────
// Ported from JRickey/gba-recomp (https://github.com/JRickey/gba-recomp),
// crates/screen/src/{color,profile,lut}.rs, © Jrickey, MIT OR Apache-2.0,
// used with permission. C++ port + the RGB888-input present path are ours.
// See THIRD_PARTY_ATTRIBUTION.md.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace gbarecomp::runtime {

// Which physical screen revision to simulate.
enum class ScreenKind {
    Raw,       // 5→8 bit replication, untouched (default; passthrough)
    Unlit,     // original reflective panel, dark viewing
    Frontlit,  // reflective panel, lit
    Backlit,   // late near-sRGB panel, clean blacks
    Classic,   // community-canonical gamma-4.0 model
};

// Display colorspace the emitted bytes are interpreted in.
enum class DisplayTarget { Srgb, DisplayP3 };

// Parse a config/env token; returns false if unrecognized.
bool screen_kind_from_name(std::string_view name, ScreenKind& out);

struct ColorSettings {
    ScreenKind    screen = ScreenKind::Raw;
    double        darken = -1.0;  // <0 = per-screen default
    DisplayTarget target = DisplayTarget::Srgb;
};

// A baked BGR555 → RGB888 table. Build once per settings change; apply per
// frame as one indexed lookup per pixel.
class ColorLut {
public:
    explicit ColorLut(const ColorSettings& settings);

    bool is_passthrough() const { return passthrough_; }

    // Transform a 240×160 RGB888 frame (R,G,B byte order, as the PPU and the
    // SDL RGB24 texture use) into `dst`. The 5-bit hardware index is
    // recovered from the top 5 bits of each channel (GBA color is BGR555;
    // our RGB888 is its 5→8 expansion). `dst` must hold width*height*3 bytes.
    void map_rgb888(const uint8_t* src, uint8_t* dst,
                    int width, int height) const;

private:
    // table_[bgr555] = {R,G,B}.
    std::unique_ptr<std::array<std::array<uint8_t, 3>, 32768>> table_;
    bool passthrough_ = false;
};

}  // namespace gbarecomp::runtime
