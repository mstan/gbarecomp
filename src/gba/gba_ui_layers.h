#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace gba {

// One synchronous, immutable frame-start presentation transaction. The caller
// owns these read-only views until render_ui_layers returns; no guest execution,
// callbacks, device time or retained guest pointers belong to the job.
struct GbaUiLayerJob {
    struct Rect { int x, y, width, height; };
    std::span<const std::uint8_t> io, vram, oam, palette;
    std::span<const Rect> regions;
    std::uint8_t world_hidden_layers = 0;
    bool world_obj_window = true;
    // Match the original UI/world operation: set OAM attr0 bit 9 in the world
    // variant. Non-affine pieces disappear; affine pieces gain double bounds.
    std::array<bool, 128> world_disable{};
};

struct GbaUiLayerOutput {
    std::span<std::uint8_t> ui_rgb, world_rgb, bg0_opaque;
};

// Mode-0 native UI and cursor-suppressed world share each background/object
// sample and window preparation. Only the union of clipped regions is written.
// The consumer chooses hidden layers and OBJ-window use for the second result;
// WIN0/WIN1, priority and blend ordering retain their hardware meanings.
// The BG0 mask is authored 4bpp tile opacity, independent of display/windows.
// No history is retained: mutations, reset and restore take effect next job.
// Unsupported layouts/foreign presentation decline before modifying output.
// A changed OBJ-window piece requires the second variant to disable OBJ windows.
bool render_ui_layers(const GbaUiLayerJob&, const GbaUiLayerOutput&);

} // namespace gba
