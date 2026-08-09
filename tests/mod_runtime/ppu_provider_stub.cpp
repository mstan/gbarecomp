// Test-only implementation of the PPU-owned foreign presentation seam.
//
// mod_runtime_tests exercises publication authorization and reset behaviour;
// ppu_smoke_tests links the real PPU implementation and validates composition.

#include <cstdint>

#include "foreign_presentation_internal.h"

namespace gba::foreign_presentation_internal {
namespace {
const std::uint16_t* g_background = nullptr;
const GbaForeignObjFocusTransform* g_focus = nullptr;
const GbaForeignScreenOverlay* g_overlay = nullptr;
}  // namespace

void set_background(const std::uint16_t* pixels) { g_background = pixels; }
const std::uint16_t* background() { return g_background; }
void set_obj_focus(const GbaForeignObjFocusTransform* focus) { g_focus = focus; }
const GbaForeignObjFocusTransform* obj_focus() { return g_focus; }
void set_screen_overlay(const GbaForeignScreenOverlay* overlay) { g_overlay = overlay; }
const GbaForeignScreenOverlay* screen_overlay() { return g_overlay; }
void clear() { g_background = nullptr; g_focus = nullptr; g_overlay = nullptr; }
}  // namespace gba::foreign_presentation_internal
