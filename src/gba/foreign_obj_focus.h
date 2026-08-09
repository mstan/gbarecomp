// foreign_obj_focus.h -- C-ABI data contract for trusted foreign-room OBJ focus.
//
// The descriptor is data only.  A game-owned plugin must publish a fully
// initialized immutable instance and keep it alive while it is published.  A
// two-element descriptor buffer is sufficient when the game needs to update
// an anchor: write the inactive element completely, then publish its address.

#pragma once

#include <stdint.h>

#define GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION 1u

// `flags == 0` is the default and preserves every unfocused OBJ exactly.  The
// explicit bit is accepted for self-documenting callers; no hide/unfocused
// mode exists in v1, because a focus transform must never remove HUD or other
// unrelated guest objects.
#define GBA_FOREIGN_OBJ_FOCUS_PRESERVE_UNFOCUSED 0x00000001u

typedef struct GbaForeignObjFocusTransform {
    uint32_t abi_version;
    // Source Link feet in guest screen coordinates and its destination in the
    // foreign virtual room. The PPU applies destination - source to matching
    // visible OBJ origins; it does not inspect guest state.
    int16_t source_link_feet_x;
    int16_t source_link_feet_y;
    int16_t destination_link_feet_x;
    int16_t destination_link_feet_y;
    // Inclusive source rectangle radii about source_link_feet. An OBJ matches
    // when either its decoded origin or its decoded bounding-box center lies
    // inside this rectangle.
    uint16_t source_radius_x;
    uint16_t source_radius_y;
    uint32_t flags;
} GbaForeignObjFocusTransform;

#if defined(__cplusplus)
static_assert(sizeof(GbaForeignObjFocusTransform) == 20,
              "foreign OBJ focus ABI must remain a fixed 20-byte descriptor");
#endif
