// foreign_presentation_internal.h -- engine-only PPU presentation seam.
//
// This is intentionally not part of the C mod ABI.  The writable storage is
// private to the PPU implementation; the mod runtime is the sole production
// caller of these C++ functions after it has checked committed-plugin policy.
#pragma once

#include <cstdint>

#include "foreign_obj_focus.h"
#include "foreign_screen_overlay.h"

namespace gba::foreign_presentation_internal {

void set_background(const std::uint16_t* pixels);
const std::uint16_t* background();
void set_obj_focus(const GbaForeignObjFocusTransform* focus);
const GbaForeignObjFocusTransform* obj_focus();
void set_screen_overlay(const GbaForeignScreenOverlay* overlay);
const GbaForeignScreenOverlay* screen_overlay();
void clear();

}  // namespace gba::foreign_presentation_internal
