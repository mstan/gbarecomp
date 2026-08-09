// foreign_obj_focus.h -- C-ABI data contract for trusted foreign-room OBJ focus.
//
// The descriptor is data only.  A game-owned plugin must publish a fully
// initialized immutable instance and keep it alive while it is published.  A
// two-element descriptor buffer is sufficient when the game needs to update
// an anchor: write the inactive element completely, then publish its address.

#pragma once

#include <stdint.h>

#define GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION 3u

// `flags == 0` is the default and preserves every unfocused OBJ exactly. The
// explicit bit is accepted for self-documenting callers.  v3 has no blanket
// hide-unfocused mode: only a source-associated nearby large prop can be
// suppressed, leaving HUD and unrelated guest objects composed normally.
#define GBA_FOREIGN_OBJ_FOCUS_PRESERVE_UNFOCUSED 0x00000001u

// Match only an OBJ origin, not its bounding-box center.  A source-mapped
// player origin can opt into this narrower association when a nearby large
// room prop would otherwise have its center under the player feet.  This is
// presentation-only filtering: unfocused objects remain composed normally.
#define GBA_FOREIGN_OBJ_FOCUS_ORIGIN_ONLY         0x00000002u

// Match only OAM tiles in the source entity's bounded VRAM allocation.  This
// is a source-backed association for composite player sprites; it leaves room
// props that overlap the same screen rectangle untouched.
#define GBA_FOREIGN_OBJ_FOCUS_SOURCE_TILE_RANGE    0x00000004u

// Suppress a large (at least 32 pixels on either axis) non-player OBJ only
// when it also falls inside this focus rectangle.  This removes a frozen room
// prop behind a translated player without touching HUD or distant guest OBJ.
#define GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_LARGE_NEARBY 0x00000008u

typedef struct GbaForeignObjFocusTransform {
    uint32_t abi_version;
    // Source Link feet in guest screen coordinates and its destination in the
    // foreign virtual room. The PPU applies destination - source to matching
    // visible OBJ origins; it does not inspect guest state.
    int16_t source_link_feet_x;
    int16_t source_link_feet_y;
    int16_t destination_link_feet_x;
    int16_t destination_link_feet_y;
    // Inclusive source rectangle radii about source_link_feet. An OBJ normally
    // matches when either its decoded origin or its decoded bounding-box center
    // lies inside this rectangle. ORIGIN_ONLY restricts it to the origin.
    uint16_t source_radius_x;
    uint16_t source_radius_y;
    uint32_t flags;
    // GBA OBJ tile units. SOURCE_TILE_RANGE requires a nonempty, in-bounds
    // half-open interval [source_obj_tile_base, base + source_obj_tile_count).
    uint16_t source_obj_tile_base;
    uint16_t source_obj_tile_count;
    // Optional second source allocation, e.g. the source player's shadow.
    // Zero count disables it; when nonzero it uses the same in-bounds rule.
    uint16_t source_aux_obj_tile_base;
    uint16_t source_aux_obj_tile_count;
} GbaForeignObjFocusTransform;

#if defined(__cplusplus)
static_assert(sizeof(GbaForeignObjFocusTransform) == 28,
              "foreign OBJ focus ABI must remain a fixed 28-byte descriptor");
#endif
