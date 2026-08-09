# Native screen overlay ABI

`GbaForeignScreenOverlay` is a deliberately small, data-only seam for a
trusted mod to draw a bounded effect over a native GBA scene. Its public
definition and ABI version live in `src/gba/foreign_screen_overlay.h`.

## Publishing and lifetime

Only the selected, committed plugin may call
`gba_mod_publish_foreign_screen_overlay`. The engine stores the descriptor
pointer; it does not copy the descriptor, pixel buffer, or alpha buffer. A
publisher must therefore fully initialize both buffers before publishing and
keep the descriptor and both buffers immutable and alive until it calls
`gba_mod_clear_foreign_screen_overlay`, replaces the descriptor, or the engine
clears foreign presentation state. Publishing, replacing, and clearing must
happen on the emulation/game thread, not concurrently with PPU rendering.

The descriptor is canonical only when `abi_version` is the current version,
both data pointers are non-null, dimensions are in `1..64`, `width <= stride
<= 64`, reserved fields are zero, and every selected alpha value is in `0..16`
(Q4). Invalid descriptors fail closed. The ABI contains no guest addresses,
callbacks, ownership transfer, or write capability.

Foreign presentation state is cleared before every plugin activation callback,
and by PPU reset and savestate deserialization. A plugin must republish a fresh
descriptor after any of those boundaries; it must never retain an assumed
active overlay across them.

## Composition contract

The overlay is clipped to the native 240x160 screen and is evaluated after
native BG/backdrop candidates but before guest OBJ candidates. Transparent
texels expose native terrain; guest OBJ (including Link and HUD objects) wins
above the overlay. With adaptive view expansion, those same native coordinates
are centered between the margins; an overlay never paints a margin. The overlay
is suppressed whenever a full foreign background is published, avoiding a
stale native effect over foreign terrain.

The overlay behaves as synthetic BG2 for the native PPU's window and color
effect rules. Native WIN0/WIN1/OBJ-window masks can hide it, and its BG2
`BLDCNT` target bits participate in normal alpha/brightness fades. This is a
presentation seam only: it does not affect collision, game state, or guest
rendering memory.
