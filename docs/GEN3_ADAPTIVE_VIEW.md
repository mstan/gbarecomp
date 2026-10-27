# Gen III adaptive overworld renderer

FireRed/LeafGreen and Ruby/Sapphire use `src/presentation/gen3`, derived from
EmeraldRecomp's read-only expanded-world renderer. Emerald keeps its existing
implementation. This is an optional game-authorized presentation adapter,
outside the GBA hardware libraries. The native renderer remains available.

Enable **Adaptive Overworld** under **Mods**, leave **Aspect ratio** set to
**Fit to screen**, and apply the selection. Landscape windows reveal more map
on either side; narrow windows reveal more above and below. The logical limits
are 569 x 854, with the usual aspect-driven desktop view expanding one axis.
Fixed 16:9, 21:9 and 32:9 are also available. The feature defaults to disabled.

The title supplies a `Profile` in `src/gen3_profile.cpp`. Supported cartridges
are FireRed USA rev0 (`41cb23d8...`), LeafGreen USA rev0 (`574fa542...`), Ruby
USA rev1 (`610b96a9...`) and Sapphire USA rev1 (`4722efb8...`). Both the runner
and catalog gate activation by the complete ROM SHA-1. Metadata provenance is
each variant's imported symbol tables and the pinned reference's fieldmap,
camera, door, object and text structure layouts. Map-group pointers are checked
against the literal in the ROM's `Overworld_GetMapHeaderByGroupAndId` routine.
No reference C code is linked or executed.

Family differences handled by the adapter:

| Data | FireRed/LeafGreen | Ruby/Sapphire |
| --- | --- | --- |
| Primary metatiles | 640 | 512 |
| Attributes | 32 bits, layer bits 29-30, pointer at tileset +20 | 16 bits, layer bits 12-15, pointer at +16 |
| Field BG order | BG1 above BG2 (buffer names reversed) | BG1 above BG2 |
| Borders | Layout-authored width and height | Repeating 2 x 2 |
| Save block | Relocated pointer | Direct address |
| Subsprites | Four-byte records with signed byte coordinates | Six-byte records with 16-bit coordinates |
| UI | Published Window array | Published standard/dialogue frame borders |

Each expanded field frame must match all 210 sampled native tile/layer probes.
Camera publication lag is resolved against VRAM within one metatile. Doors
accept only their exact ROM-table/palette tile patterns. Missing or unsupported
data restores native presentation. Connections extend beyond the guest's live
padding only when their tilesets match the currently resident graphics.

Active actors use live software sprite data, verified against hardware OAM,
VRAM and PAL. Dormant actors use ROM idle graphics at their template or last
observed position, only when the matching palette is resident. Story flags,
explicit invisibility and active identities suppress dormant copies. Scene
changes and state restoration invalidate pose history. AI, spawn range,
trainer triggers, collisions and scripts retain native behavior; distant
actors are presentation only. Special effects, reflections, weather and
unqualified graphics fall back to native visibility.

Field menus move their existing text, borders and cursors to the expanded
edges. Full-screen interfaces and battles keep their native image centered.
Ruby/Sapphire's cursor uses normal and OBJ-window sprites. While its field
windows are open, the adapter samples a private native UI raster and a native
world raster without that cursor. It publishes only opaque relocated UI pixels
and restores the vacated source rectangles. The optional RGB555 UI provider
runs only in expanded presentation; native rendering and guest state stay
unchanged. This also preserves the cursor stencil when a portrait menu moves
to a different scanline.
The providers never write guest memory, saves, ROM or hardware registers, and
never execute extra guest code. Disabling the mod overrides stale generic
view requests. The shared callback runs in normal and TCP execution.

`GbaGen3View.cmake` registers `<Target>_view_tests` for all four game targets.
These synthetic tests cover primary/secondary tiles, all layer assignments,
eight landscape/portrait sizes, connected map coverage, unchanged native
samples and guest memory, unsupported scenes and malformed pointers, live
and dormant NPC visibility, story hiding, and both families' UI anchoring.
They do not establish whole-game content coverage.

`tools/gen3_view_smoke.py` compares strict native/adaptive execution from an
isolated current-build checkpoint. It accepts `--exe`, `--bios`, `--rom`,
`--catalog`, `--game`, `--output`, and either `--state` or `--prepare --save`.
It compares native pixels and RAM/VRAM/PAL/OAM, requires nonblack world margins,
and retains screenshots, coverage, captures and a report. All writable data
must be directed to non-C: storage. The source SRAM and fixtures remain intact.
Expanded presentation is compared with the existing native renderer; this
does not claim a new independent mGBA qualification of the base engine.

`<Target>_view_capture_check <capture-dir> <ROM> [width] [--height N]`
reconstructs the map and objects from captured memory, compares native pixels,
and counts decoded and actually displayed object-margin pixels separately.
`--require-visible-objects` and `--require-dormant-objects` turn missing fixture
coverage into failures. `tools/gen3_view_resize_smoke.py` accepts the same
path arguments plus `--catalog` and tests eight real SDL window sizes, including
portrait, odd widths and a fixed-aspect selection. It requires expanded scenery
and strict execution in every case. Tests use muted, isolated processes.

`--outdoors` builds an isolated renderer fixture through the game's own map
loader. It bypasses story/doorway prerequisites and does not set completion
flags; its report explicitly does not claim successful quest or doorway checks.
`--menu` verifies the relocated Start text/cursor. Resize checks can additionally
use `--ui-center 44` (FRLG) or `--ui-center 56` (RS) with a Start-menu checkpoint
to compare live pixels in all window geometries.

Validated on 2026-10-23 for all four supported cartridges:

| Check | Result |
| --- | --- |
| Four title profile suites | Passed |
| Mod runtime and shared PPU suites | Passed; PPU also passed with LLE scanlines |
| Native versus expanded outdoor runs | 23 equal native image samples and 10 equal memory checkpoints per title |
| Start menus | 8 exact live UI comparisons per title |
| Actual SDL menu resizes | 32 cases passed, each with 4,608 matching text/cursor channels |
| 240 x 854 NPC captures | All 210 camera probes and native pixels match; visible dormant actors in every title |

Tall-view final visible NPC pixels were 526 (FireRed), 526 (LeafGreen), 903
(Ruby), and 853 (Sapphire). All paired game runs reported fully static execution,
with zero dispatch misses, interpreted instructions or healed functions.
Coverage is representative overworld/UI qualification, not an exhaustive audit
of every map, battle, weather mode or special effect. Local captures, fixture
states and runnable builds are under `G:/Temp/Codex/gbarecomp/gen3-adaptive`.
