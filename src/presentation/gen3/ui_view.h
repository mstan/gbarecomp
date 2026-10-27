#pragma once
#include "field_view.h"

namespace gen3 {
// Move the game's live BG0 window rectangles, including their authored
// borders/text/cursors. No menu reconstruction or guest state changes.
class UiView {
public:
    void prepare(const ViewMemory& memory, const FieldView& field, int width, int height);
    int sample(int bg, int x, int y, int* source_x, int* source_y) const;
    int pixel(int x, int y, std::uint16_t* color) const;
private:
    struct Rect { int x, y, w, h, dx, dy; };
    std::array<Rect, 32> windows_{};
    int count_ = 0;
    bool active_ = false;
    bool composed_ = false;
    std::array<std::uint8_t, 240*160*3> native_ui_{}, native_world_{};
    std::array<bool, 240*160> opaque_{};
    void compose(const ViewMemory& memory, unsigned cnt);
};
}
