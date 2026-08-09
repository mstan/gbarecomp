// foreign_screen_overlay.h -- bounded, data-only native-screen overlay ABI.
//
// A committed trusted plugin may publish one immutable portal/effect rectangle.
// The PPU composites it over the guest's native backgrounds, then performs the
// ordinary guest OBJ pass, so player and HUD objects remain in front.  This
// descriptor has no guest-memory addresses, callbacks, tiles, or write path.

#pragma once

#include <stdint.h>

#define GBA_FOREIGN_SCREEN_OVERLAY_ABI_VERSION 1u
#define GBA_FOREIGN_SCREEN_OVERLAY_MAX_WIDTH 64u
#define GBA_FOREIGN_SCREEN_OVERLAY_MAX_HEIGHT 64u

typedef struct GbaForeignScreenOverlay {
    uint32_t abi_version;
    // Row-major BGR555 texels and matching Q4 opacity values. `alpha_q4[i]`
    // is in [0,16], where zero leaves the native pixel untouched and 16 is
    // opaque. Both arrays are immutable and owned by the publishing plugin.
    const uint16_t* pixels;
    const uint8_t* alpha_q4;
    // Top-left position in the native 240x160 screen domain. Negative values
    // are legal solely for clipping at the native viewport edge.
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    // Texels between successive rows. It may exceed width only up to the
    // fixed native-overlay maximum; it must never be smaller.
    uint16_t stride;
    uint16_t reserved16;
    uint32_t reserved32;
} GbaForeignScreenOverlay;

#if defined(__cplusplus)
static_assert(sizeof(GbaForeignScreenOverlay) ==
                  (sizeof(void*) == 8 ? 40u : 28u),
              "foreign screen overlay ABI layout changed");
#endif
