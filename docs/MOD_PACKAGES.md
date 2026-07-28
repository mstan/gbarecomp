# GBARecomp mod packages and trusted plugins

Mod support is an explicit per-game build feature:

```cmake
set(GBARECOMP_ENABLE_MODS ON CACHE BOOL "" FORCE)
add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)
```

The default is `OFF`. Opting in also enables recomp-ui's Mods surface, which
still remains hidden unless the game supplies a catalog through
`RunOptions::mod_game_id`.

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

On Play, the runtime verifies the selected stock ROM, resolves and persists the
feature state, runs reset callbacks, and activates the committed plugins. A
game that moves widescreen from generic Display settings into Mods should keep
the generic launcher control hidden and set
`RunOptions::mod_owns_adaptive_view`; the package state then remains
authoritative over stale config, CLI, or environment values.

The initial operation vocabulary is intentionally limited to trusted
activation plugins. Future guarded ROM writes, asset overlays, and hooks must
retain the same pre-boot validation and no-arbitrary-code model.
