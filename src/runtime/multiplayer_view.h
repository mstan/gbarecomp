#pragma once
#include "gba_ppu.h"
#include <memory>
#include <string_view>

namespace gbarecomp {
struct ExtendedViewFrameInfo;

enum class GbaNetplayView : unsigned { Native, Wide16x9, Wide21x9, Wide32x9, Adaptive };

// The widest netplay view any game was validated at before the engine's
// render capacity grew past 576 (GbaPpu::kMaxRenderWidth was 576). It is the
// default game ceiling so raising engine capacity never silently widens an
// existing game's netplay view; a game opts into more via max_width.
inline constexpr std::uint32_t kGbaNetplayDefaultMaxViewWidth = 576;

// Explicit game authorization: these callbacks may READ the active machine
// and author host pixels only. Guest patches/camera changes do not belong here.
// Adaptive can be disabled independently without changing single-player policy.
struct GbaNetplayViewPolicy {
    bool supported = false;
    bool adaptive_supported = false;
    void (*init)(std::uint32_t left, std::uint32_t right) = nullptr;
    void (*frame)(const ExtendedViewFrameInfo*) = nullptr;
    void (*reset)() = nullptr;
    // Game-validated widest logical view, in [240, GbaPpu::kMaxRenderWidth].
    // Adaptive widths clamp to it; a fixed view wider than it is rejected.
    std::uint32_t max_width = kGbaNetplayDefaultMaxViewWidth;
};
GbaNetplayView parse_gba_netplay_view(std::string_view);
void validate_gba_netplay_view(GbaNetplayView, const GbaNetplayViewPolicy&);
// Fixed views ignore the drawable. Adaptive follows the drawable aspect and
// clamps to [240, min(max_width, GbaPpu::kMaxRenderWidth)].
unsigned gba_netplay_view_width(GbaNetplayView, int drawable_width = 0, int drawable_height = 0,
                                std::uint32_t max_width = kGbaNetplayDefaultMaxViewWidth);

// One host display for the local seat. The canonical PPUs stay native-sized;
// neither this surface nor the window geometry enters a session snapshot/hash.
class GbaNetplayPresentation final : public gba::GbaPpu::PresentationObserver {
public:
    explicit GbaNetplayPresentation(GbaNetplayViewPolicy, bool affine_filter = false);
    ~GbaNetplayPresentation();
    void request_width(unsigned width);
    const std::uint8_t* pixels() const;
    unsigned width() const;
    void scanline(const gba::GbaPpu&, std::uint32_t, std::uint16_t,
                  const std::uint8_t*, const std::uint8_t*,
                  const std::uint8_t*, const std::uint8_t*) override;
    void frame_ready() override;
    void restored() noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
