#pragma once

#include "host_window.h"
#include "recomp_runtime_ui.h"

namespace gbarecomp {

inline constexpr char kTouchControlsUiKey[] = "input.touch_controls";

inline RecompRuntimeUiItem touch_controls_ui_item() {
    RecompRuntimeUiItem item{};
    item.key = kTouchControlsUiKey;
    item.section = "Input";
    item.label = "Touchscreen controls";
    item.description = "Show on-screen game buttons. Hiding them keeps Settings shortcuts and game touch gestures available.";
    item.type = RECOMP_RUNTIME_UI_BOOL;
    return item;
}

// Use the host's existing choice and persistence; hiding the buttons must not
// disable the touch gestures or controller shortcuts that open this menu.
inline int touch_controls_ui_get(HostWindow* window, int* value) {
    if (!window || !value || !window->touch_controls_available()) return 0;
    *value = window->touch_pad_visible() ? 1 : 0;
    return 1;
}
inline int touch_controls_ui_set(HostWindow* window, int value) {
    if (!window || !window->touch_controls_available()) return 0;
    window->set_touch_pad_visible(value != 0);
    return 1;
}

}  // namespace gbarecomp
