# Browser bundle

This is the supported browser workflow for an exported, statically covered
game. It produces a WebAssembly bundle with the web host, audio worklet and
IDBFS-backed saves. The recompiler is not available in the browser, so generate
the game and BIOS with a native build first.

## Prerequisites

- Bash (the script uses arrays and `set -euo pipefail`)
- Python 3.9 or newer
- CMake and a native C++ toolchain
- Emscripten SDK, activated with `source ~/emsdk/emsdk_env.sh`

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
  build/generated_bios /path/to/game.gba bios/gba_bios.bin
```

The script syntax is `build_web.sh <project> <generated-bios> [rom] [bios]
[config]`. ROM and BIOS may be empty when supplied separately at runtime. The
optional fifth argument selects the runtime configuration. Without it, the
build uses `<project>/game.toml` if present, otherwise runtime defaults.
`tools/cli.py build --config` does not copy its input into the exported project:
pass that original file as the fifth argument when it lives elsewhere. A missing
explicit configuration is an error. ROM and BIOS arguments may be empty strings
when those assets are supplied separately.

## Serve and test

Start the static server with the headers required for pthreads:

```sh
python3 packaging/web/serve.py /path/to/game-recomp/web 8080
curl -I http://127.0.0.1:8080/game.wasm
```

The response should include `Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp` and the WebAssembly content type.
Open `http://127.0.0.1:8080/` in a browser. For an automated smoke test use
`index.html?autostart=1`; autoplay audio may require a real click. Useful parity
URLs are:

```text
?args=--window%20--frames%201800%20--dump-png%20/data/final.png&env=GBARECOMP_SELFHEAL_RECOMPILE=0
?args=--no-window%20--frames%201800%20--dump-png%20/data/final.png&env=GBARECOMP_SELFHEAL_RECOMPILE=0
```

For a test harness, collect `/data/final.png`,
`/recomp_coverage_BPEE.json` and `/recomp_master_misses_BPEE.toml.frag` from
`Module.FS` in the exit callback. A passing test requires guest exit code 0 and
the expected final output; a closed browser process alone is insufficient.

The build always writes `runtime.toml`, including an empty configuration when
there is no input, so rebuilding cannot retain stale settings. `runtime_config.py`
exports only the scalar fields understood by `runtime.cpp::apply_toml_file`:
game name, ROM/BIOS hashes, BIOS options, save type/size, video options and audio
shadow. It resolves the default-region overlays under the source's `config/`
directory in the same order as the runtime. Recompiler declarations, comments
and local ROM/BIOS/save paths are omitted.

The bootstrap fetches this file before starting the game and puts it at
`/data/runtime.toml`. ROM and BIOS remain at `/data/game.gba` and
`/data/gba_bios.bin`; browser battery saves and states retain their
`/saves/<ROM SHA-1>/` paths. Runtime command-line options still override config
values. Deploy `runtime.toml` alongside the rest of the bundle; a failed fetch
stops startup instead of silently discarding settings. Its digest is included
in `manifest.json`.

Configuration regression checks (no ROM or SDK required):

```sh
python3 packaging/tests/test_web_runtime_config.py
node packaging/tests/web_config_bootstrap_test.js
```
