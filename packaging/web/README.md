# Browser bundle

This is the supported browser workflow for an exported, statically covered
game. It produces a WebAssembly bundle with the web host, audio worklet and
IDBFS-backed saves. The recompiler is not available in the browser, so generate
the game and BIOS with a native build first.

**The bundle contains no ROM and no BIOS.** Each player picks their own dump of
the game ROM and their own GBA BIOS in the page. The ROM must match the SHA-1
this bundle was built for; both files are kept in that browser's IndexedDB for
the site, so a player picks them once. Nothing is uploaded anywhere. A bundle
built this way contains only code and settings and can be published.

## Prerequisites

- Bash (the script uses arrays and `set -euo pipefail`)
- Python 3.9 or newer
- CMake and a native C++ toolchain
- Emscripten SDK, activated with `source ~/emsdk/emsdk_env.sh`
- `sha1sum`, `shasum` or Python 3 (used for SHA-1 checks; the build fails if
  none is available)

The runtime and game are built in separate Emscripten trees. Every object and
the final link use `-pthread` and `-mtail-call`; omitting either flag causes
configuration or compilation/link errors.

## Full build

From the repository root, build the native recompiler and generate the BIOS:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGBARECOMP_COMPILER_CACHE=OFF
cmake --build build --target gba_recompile -- -j10
build/gba_recompile --bios bios/gba_bios.bin --config bios/gba_bios.toml \
  --out build/generated_bios
```

Export the game project, then package it for the browser:

```sh
GBARECOMP_CORE="$PWD/build/gba_recompile" python3 tools/cli.py build \
  --rom /path/to/game.gba --output /path/to/game-recomp \
  --config /path/to/game.toml --force
bash packaging/web/build_web.sh /path/to/game-recomp \
  build/generated_bios /path/to/game.gba "" /path/to/game.toml
```

Syntax:

```text
build_web.sh [--dev] [--embed-private-rom] <project> <generated-bios> [rom] [bios] [config]
```

- `[config]` selects the runtime configuration. Without it the build uses
  `<project>/game.toml` if present, otherwise runtime defaults.
  `tools/cli.py build --config` does not copy its input into the exported
  project: pass that original file here when it lives elsewhere. A missing
  explicit configuration is an error.
- The expected ROM SHA-1 comes from `[rom].sha1` in that configuration. `[rom]`
  is optional; when given it is only hashed (never copied): it must match
  `[rom].sha1`, or supplies the hash when the configuration has none. The build
  fails when neither provides one.
- `[bios]` is optional and only checked (16 KiB; an unexpected SHA-1 is a
  warning, as in the native runtime). The expected BIOS SHA-1 is `[bios].sha1`,
  or the standard GBA BIOS hash.
- Pass `""` for an optional positional argument you want to skip.

The output directory is `<project>/web` (override with
`GBARECOMP_WEB_OUT_DIR`). Each build removes any `game.gba`, `gba_bios.bin`,
`rom_sha1.js`, `build_info.js` and `PRIVATE-*` file left there by an earlier
build, so a directory can never keep serving an old ROM or identity.

`build_info.js` carries the expected ROM and BIOS SHA-1 and the build flags;
`manifest.json` records the source revision (`unknown` for a checkout without
git metadata, such as a release tarball), the emcc version, the expected hashes
and a SHA-256 of every bundle file and generated input.

### Developer-only options

`--embed-private-rom` copies the ROM and BIOS into the bundle as
`PRIVATE-game.gba` and `PRIVATE-gba_bios.bin`, next to a
`PRIVATE-DO-NOT-PUBLISH.txt` notice, in `<project>/web-PRIVATE` by default.
The page then loads them without asking (still checking the ROM SHA-1). This
is for local testing only. **Never upload, deploy, share or commit a
`--embed-private-rom` bundle**: it contains copyrighted files.

`--dev` enables developer URL parameters, and only when the page is opened
from `localhost`, `127.0.0.1` or `[::1]`:

- `?args=--no-window%20--frames%201800` replaces the extra runtime arguments
  (default `--window`);
- `&env=KEY=VALUE,KEY2=VALUE2` sets environment variables;
- `&rom=` / `&bios=` pick other served file names in an embedded bundle.

A normal bundle ignores these parameters (the log says so). No parameter can
change the expected ROM SHA-1.

## Serve and play

Start the static server with the headers required for pthreads:

```sh
python3 packaging/web/serve.py /path/to/game-recomp/web 8080
curl -I http://127.0.0.1:8080/game.wasm
```

The response should include `Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp` and the WebAssembly content type.
Any static host works if it sends those two headers and serves the page over
https (or localhost): SharedArrayBuffer and the SHA-1 check need a secure,
cross-origin-isolated context.

Open `http://127.0.0.1:8080/`, choose the ROM and the BIOS, then Start. A wrong
ROM is refused with its SHA-1 and the expected one; a 16 KiB file offered as
the ROM is recognised as a BIOS. "Forget ROM/BIOS" removes both from the
browser (saves are kept). Without IndexedDB (some private windows) the files
are used for that page only. `index.html?autostart=1` starts as soon as stored
files are available; autoplay audio may require a real click.

### Browser behaviour

- Saves: battery saves and save states live under `/saves/<ROM SHA-1>/` on an
  IDBFS mount synced by `save_store.js`. The status line says whether the
  latest change is stored in the browser or exists only in memory; the page
  warns before unloading, and Restart asks, while anything the runtime or the
  player wrote is not yet stored. Save changes made before Start are queued in
  order and applied before the game reads its save; the log reports whether
  they were stored.
- Input: keyboard bindings from `keybinds.ini` / `config.ini [KeyMap]` accept
  `Ctrl+`, `Alt+` and `Shift+` chords, matched like the native backend
  (hotkeys need their exact modifiers). A modifier key bound to a game button
  (Select is right Shift by default) never turns a key into a chord, so
  Select+F1 loads slot 1. A chord owns its key: Alt+Enter toggles fullscreen
  without pressing Start.
- Touch: on touch devices (or with `--touch-emulation`) the page draws a
  gamepad over the game; its presses are GBA buttons. Every other touch on the
  game goes to the runtime's touch model (`TouchHub`) for games with a touch
  policy; an unclaimed long press, three-finger tap or Back reveals the page
  controls. The pad choice is remembered per game. With emulation the mouse is
  a finger, right/middle click are two/three-finger taps and Backspace is Back.
- Host overlays drawn by a game's touch policy are replayed on a 2D canvas
  over the image (overlay text is unavailable). Haptics use
  `navigator.vibrate` where the browser has it. Portrait presentations can be
  anchored to the top edge; the orientation policy is applied as a screen
  orientation lock while fullscreen.
- Lifecycle: hiding the page is the browser's "background": the runtime
  flushes the battery save, writes its suspend state and holds the guest until
  the page is visible again, like a mobile app.

## Configuration

The build always writes `runtime.toml`, including an empty configuration when
there is no input, so rebuilding cannot retain stale settings. `runtime_config.py`
exports only the scalar fields understood by `runtime.cpp::apply_toml_file`:
game name, ROM/BIOS hashes, BIOS options, save type/size, video options and audio
shadow. It resolves the default-region overlays under the source's `config/`
directory in the same order as the runtime. Recompiler declarations, comments
and local ROM/BIOS/save paths are omitted.

The bootstrap fetches this file before starting the game and puts it at
`/data/runtime.toml`. The player's ROM and BIOS are written to `/data/game.gba`
and `/data/gba_bios.bin`; browser battery saves and states keep their
`/saves/<ROM SHA-1>/` paths. Runtime command-line options still override config
values. Deploy `runtime.toml` alongside the rest of the bundle; a failed fetch
stops startup instead of silently discarding settings.

## Tests

Unit tests (no ROM, SDK or browser; run by CTest when node is found):

```sh
for t in tests/web/*_test.js; do node "$t"; done
node packaging/tests/web_config_bootstrap_test.js
python3 packaging/tests/test_web_runtime_config.py
```

### Browser acceptance tests

`tests/web/test_game.py` and `tests/web/test_save.py` drive a real headless
Chrome/Chromium through the DevTools protocol. They are not part of CTest: they
need a built game, its ROM and BIOS, and a browser. They boot the runtime
through `?args=`, so build a developer bundle that carries its own files:

```sh
pip install websocket-client
bash packaging/web/build_web.sh --dev --embed-private-rom /path/to/game-recomp \
  build/generated_bios /path/to/game.gba bios/gba_bios.bin /path/to/game.toml
python3 packaging/web/serve.py /path/to/game-recomp/web-PRIVATE 18083
python3 tests/web/test_game.py 1800 /tmp/gbr-game --url http://127.0.0.1:18083/
python3 tests/web/test_save.py /tmp/gbr-save --url http://127.0.0.1:18083/
```

The browser is found through `CHROME=/path/to/browser` (or
`GBARECOMP_CHROME`), then the standard Chrome, Chromium and Edge install
locations on Windows, macOS and Linux, then `PATH`.
