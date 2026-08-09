# GBARecomp mod packages and trusted plugins

Mod support is an explicit per-game build feature:

```cmake
set(GBARECOMP_ENABLE_MODS ON CACHE BOOL "" FORCE)
add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)
```

The default is `OFF`. Opting in also enables recomp-ui's Mods surface, which
still remains hidden unless the game supplies a catalog through
`RunOptions::mod_game_id`.

## Trusted native mod state

Package selection remains `mods/state.toml`; it is not save data. Trusted
game-linked providers that need mutable native state register at startup with
`gbarecomp::debug::register_mod_state_provider` into the bounded process-wide
`ModStateRegistry`. Each provider has a stable ID (copied by the registry),
schema number, and a non-mutating preflight callback. Registration closes on
the first save/load operation, so the catalog cannot change mid-session. A save state with an
active registry includes a tagged `MODS` section containing its exact catalog
and provider payloads. Loading validates that complete section before any CPU,
memory, or device state is restored, so a missing provider or catalog/schema
mismatch leaves guest state untouched.

The snapshot container remains version 2. This is deliberate compatibility
policy: v2 snapshots made before `MODS` continue to load when the runtime has
an empty mod-state catalog. Once a game registers native providers, old v2
snapshots without `MODS` are rejected rather than guessing native state.

The original v1 seven-section container is also accepted for an empty catalog,
after complete container framing (exactly `CPU0`, `BUS0`, `IO_0`, `AUD0`,
`SAV0`, `PPU0`, and `META`, with no trailing bytes). A game with any registered
provider rejects v1 before guest restoration because v1 has no `MODS` identity
or migration record. New compatibility must be an explicit migration; loaders
must not infer provider payloads or hard-code historical section lengths.

For durable per-slot native state, use `write_mod_state_slot` and
`read_mod_state_slot` with a directory owned by the game. They never use guest
save padding or `state.toml`. Slot keys are canonical lower-case ASCII and
path-like keys are rejected. The on-disk envelope has an exact identity string
(normally ROM plus catalog identity), CRC32, same-directory `.tmp` replacement,
and `.bak` recovery after an interrupted/corrupt primary write.
Drive-relative directory spellings such as `F:slot` are rejected before any
directory is created; callers must use either a normal relative directory or a
fully rooted drive path such as `F:/game/mod-state`.

Provider `restore` receives only payloads that its matching `preflight`
accepted. It must be deterministic, consume the full payload, and cannot
report a recoverable error; a violation aborts rather than continuing after a
partially restored guest world.

## Product and trust model

- A **package** is an installation, update, provenance, and trust boundary.
- A **feature** is independently enabled and may expose boolean, choice, or
  bounded-integer options.
- A **trusted plugin** is game-owned native behavior statically linked into the
  executable and registered under a stable ID.

`.gbamod` files are ZIP archives with a root `manifest.toml`. Archives contain
data only: they cannot provide native code, DLLs, symbols, or library paths.
The loader accepts stored and DEFLATE entries, verifies CRCs, rejects encrypted
or unsafe paths, caps archives at 4096 files and 256 MiB expanded, stages
extraction, validates the manifest, and publishes a version atomically.

Installed packages live beside the executable:

```text
mods/
  state.toml
  packages/
    example.display/
      1.0.0/
        manifest.toml
```

Games may preload built-in packages by copying the same directory layout after
the build. Presentation enhancements should default to disabled.

## Manifest format 1

```toml
format_version = 1
id = "example.display"
version = "1.0.0"
name = "Example Display Enhancements"
author = "Example Author"
description = "Game-specific presentation features."
license = "MIT"
resolver = "declarative"
save_compatibility = "shared"

[[target]]
game_id = "example-game-us"
rom_sha1 = "0123456789abcdef0123456789abcdef01234567"

[[feature]]
id = "adaptive-view"
name = "Adaptive Widescreen"
description = "Reveal more world as the host aspect widens."
group = "Display"
default_enabled = false

[[plugin]]
feature = "adaptive-view"
id = "example.adaptive-view"
```

### User-owned required assets

An enabled package may require a separately dumped, user-owned file without
including it in the `.gbamod` archive. Add one `[[asset]]` record per exact
source image:

```toml
[[asset]]
feature = "adaptive-view"
id = "zelda1-rom"
name = "The Legend of Zelda ROM"
sha1 = "0123456789abcdef0123456789abcdef01234567"
size = 131088
extensions = ["nes"]
purpose = "Used by the trusted foreign-world feature; this file is not installed or redistributed."
```

`feature` names the owning manifest feature and is required. `id` is a
package-scoped stable lowercase identifier. `sha1` and `size` are strict exact
checks; SHA-1 is currently the supported content hash. Extensions are picker
hints only and are lowercase alphanumeric suffixes without a dot. `purpose` is
shown with missing-asset diagnostics. A package can declare the asset while
disabled: it becomes required only when its owning feature is enabled and its
game-ID/ROM-SHA-1 target matches. Unknown owners and duplicate asset IDs reject
the manifest.

The Mods provider resolves assets during its existing **Play/Commit** action.
It refuses absent, stale, wrong-sized, or wrong-hash files and opens the normal
file picker where available. Only the chosen local path is written to
`mods/state.toml`; neither the file bytes nor a copy of the asset enters the
package or repository. A stale remembered path is revalidated before every
commit.

The target combines a stable game ID with the same lowercase ROM SHA-1 used by
the runtime's stock-image gate. Resolution fails before boot if an enabled
feature targets another image, names a missing trusted plugin, or collides with
another feature claiming the same plugin ID.

Options follow recomp-ui's feature schema:

```toml
[[option]]
feature = "example-feature"
id = "mode"
label = "Mode"
type = "choice"
default = "stock"

[[option.choice]]
value = "stock"
label = "Stock"

[[option.choice]]
value = "enhanced"
label = "Enhanced"
```

Supported types are `boolean`, `choice`, and bounded `integer`.

## Plugin registration

```cpp
#include "mod_runtime.h"

static void reset_view() {
    gba_mod_set_adaptive_view_enabled(0);
}

static void enable_view() {
    gba_mod_set_adaptive_view_enabled(1);
}

GBA_MOD_CONSTRUCTOR(register_display_mods) {
    gba_mod_register_reset_callback(reset_view);
    gba_mod_register_activation_plugin(
        "example.adaptive-view", enable_view);
}
```

A trusted plugin can retrieve a committed required asset path, but only after
the package has passed its target and asset validation:

```cpp
const char* zelda_rom = gba_mod_required_asset_path(
    "example.foreign-world", "zelda1-rom");
// null means disabled, uncommitted, wrong target, or invalid/missing asset.
```

The returned path is runtime-owned and stays valid until the next mod-runtime
initialize or commit cycle. Plugins must open it themselves; the mod runtime
does not expose or retain asset bytes.

### Trusted foreign presentation data

An enabled, committed plugin may publish immutable presentation data without
letting the PPU call game code. `gba_mod_publish_foreign_background()` accepts
a stable native 240x160 BGR555 buffer. `gba_mod_publish_foreign_obj_focus()`
accepts a `GbaForeignObjFocusTransform` from `foreign_obj_focus.h`: a bounded
source Link-feet anchor, destination virtual anchor, inclusive source radii,
and an optional bounded uniform Q8.8 scale. The PPU translates only visible
OBJs whose decoded origin or bounding box center is inside that rectangle;
when `SOURCE_TILE_RANGE` selects a source player allocation, it may also
nearest-neighbor scale its non-affine body/shadow OBJs around the common feet
anchor. Unfocused OBJs are preserved by default; a source-tile-bounded
descriptor may explicitly suppress source-playfield OBJs, including the
full-frame policy that retains only its declared HUD priority class. Terrain,
guest OAM, and collision are never modified. Affine OBJs retain their
guest-authored scale.

Both endpoints are authorized only for the currently committed plugin ID and
are cleared before each activation callback, on PPU reset, and on savestate
load. The descriptor is neither guest state nor snapshot state. A game adapter
derives the source feet anchor from its source-mapped player position minus
guest scroll, keeps the descriptor immutable while published, and may
double-buffer descriptors before publishing the new address. The PPU performs
no callback, allocation, guest write, or OAM mutation for either endpoint.

### Trusted function-entry hooks

For a reviewed `[[mod_function_hook]]` in the recompiler TOML, a plugin may
replace that guest call without permanently patching ROM bytes:

```cpp
#include "mod_function_hooks.h"

static int replace_draw(uint32_t addr, int thumb, ArmCpuState* cpu) {
    (void)addr; (void)thumb;
    cpu->R[0] = 1;                 // callback-authored guest result
    cpu->R[15] = cpu->R[14] & ~1u; // return to the guest caller
    return 1;                      // handled: skip original body
}

static void activate_replace_draw() {
    gba_mod_set_function_hook_enabled("example.replace-draw", 1);
}

GBA_MOD_CONSTRUCTOR(register_replace_draw) {
    gba_mod_register_function_entry_plugin("example.replace-draw",
        0x08003000u, 1, replace_draw);
    gba_mod_register_activation_plugin("example.replace-draw",
        activate_replace_draw);
}
```

Registration starts disabled, and `mod_runtime_activate_plugins()` disables
all entry hooks before it runs reset/activation callbacks. Returning `0` from a
callback declines that invocation, discards any callback CPU writes, and
permits another matching trusted callback or the original guest body. Static
ROM functions require an exact `[[mod_function_hook]]` TOML allowlist entry;
the generated-code audit fails if that entry drifts from an emitted root.
Interpreter fallback and self-healed dynamic overlays have no static TOML
corpus, so their narrow allowlist is the trusted, statically linked registry
itself (`id`, aligned address, mode, callback). The same guard is used in all
three paths; callbacks must therefore set the complete desired CPU state,
especially R15.

On Play, the runtime verifies the selected stock ROM, resolves and persists the
feature state, runs reset callbacks, and activates the committed plugins. A
game that moves widescreen from generic Display settings into Mods should keep
the generic launcher control hidden and set
`RunOptions::mod_owns_adaptive_view`; the package state then remains
authoritative over stale config, CLI, or environment values.

The initial operation vocabulary is intentionally limited to trusted
activation plugins. Future guarded ROM writes, asset overlays, and hooks must
retain the same pre-boot validation and no-arbitrary-code model.
