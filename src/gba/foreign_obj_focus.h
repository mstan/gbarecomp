// foreign_obj_focus.h -- C-ABI data contract for trusted foreign-room OBJ focus.
//
// The descriptor is data only.  A game-owned plugin must publish a fully
// initialized immutable instance and keep it alive while it is published.  A
// two-element descriptor buffer is sufficient when the game needs to update
// an anchor: write the inactive element completely, then publish its address.

#pragma once

#include <stdint.h>

#define GBA_FOREIGN_OBJ_FOCUS_ABI_VERSION 5u

// Q8.8 unit scale for source-tile-matched, non-affine OBJs.  A descriptor
// scale of zero is deliberately an identity scale, so zero-initialized
// descriptors remain safe while every non-identity request is bounded by the
// trusted runtime.  The scale is applied around the Link-feet anchor, which
// keeps independently decoded body, equipment, and shadow OBJs together.
#define GBA_FOREIGN_OBJ_FOCUS_SCALE_IDENTITY_Q8_8 256u
#define GBA_FOREIGN_OBJ_FOCUS_SCALE_MIN_Q8_8      128u
#define GBA_FOREIGN_OBJ_FOCUS_SCALE_MAX_Q8_8      256u

// `flags == 0` is the default and preserves every unfocused OBJ exactly. The
// explicit bit is accepted for self-documenting callers. The default has no
// blanket hide mode; callers must explicitly opt into one of the bounded
// source-associated suppression policies below.
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

// Suppress any non-player OBJ that is geometrically associated with the
// frozen source playfield.  SOURCE_TILE_RANGE still lets the player body and
// optional player-item/shadow allocations through, while HUD and distant OBJ
// retain normal composition.
#define GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_NEARBY_NONMATCHING 0x00000010u

// Suppress every non-source-tile OBJ across the foreign frame, except the
// explicitly classified HUD priority band. This is intentionally valid only
// together with SOURCE_TILE_RANGE: the plugin must first identify the exact
// player (and optional player-item/shadow) allocations it wishes to retain.
// It prevents a native room prop from surviving merely because it is farther
// from Link than the local association radius.
#define GBA_FOREIGN_OBJ_FOCUS_SUPPRESS_NONMATCHING_EXCEPT_HUD_PRIORITY 0x00000020u

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
    // Uniform source-tile OBJ scale in Q8.8. Zero means identity (1.0).  The
    // PPU uses nearest-neighbor source sampling and never changes guest OAM,
    // terrain, HUD, or collision coordinates. Affine OBJs are translated but
    // retain their guest-authored affine transform.
    uint16_t source_obj_scale_q8_8;
    // The native HUD OAM capture uses priorities 0 and 1, while room/player
    // playfield OBJ use priority 2. This bounded upper inclusive class is
    // consulted only by SUPPRESS_NONMATCHING_EXCEPT_HUD_PRIORITY.
    uint8_t hud_obj_priority_max;
    uint8_t reserved;
} GbaForeignObjFocusTransform;

#if defined(__cplusplus)
static_assert(sizeof(GbaForeignObjFocusTransform) == 32,
              "foreign OBJ focus ABI must remain a fixed 32-byte descriptor");
#endif
