# Build and Run a ROM in the Browser

The browser bundle never contains the game ROM or the GBA BIOS. Players choose
their own files in the page; the ROM is checked against the SHA-1 the bundle
was built for, and both files stay in that browser (IndexedDB) so they are
picked only once. A normal bundle is therefore safe to publish. Details:
`packaging/web/README.md`.

From the project root, activate Emscripten and build the native recompiler:

```sh
source ~/emsdk/emsdk_env.sh

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGBARECOMP_COMPILER_CACHE=OFF
cmake --build build --target gba_recompile -- -j10

build/gba_recompile --bios bios/gba_bios.bin --config bios/gba_bios.toml \
  --out build/generated_bios
```

Export the ROM and build the browser bundle. The ROM argument is only hashed
to confirm the expected SHA-1 (`[rom].sha1` in the game config); it is not
copied:

```sh
GBARECOMP_CORE="$PWD/build/gba_recompile" python3 tools/cli.py build \
  --rom /path/to/game.gba \
  --output /path/to/game-recomp \
  --config /path/to/game.toml \
  --force

bash packaging/web/build_web.sh /path/to/game-recomp \
  build/generated_bios /path/to/game.gba "" /path/to/game.toml
```

Serve the built files on port 8080:

```sh
python3 packaging/web/serve.py /path/to/game-recomp/web 8080
```

Open http://127.0.0.1:8080/, choose your ROM and GBA BIOS, then press Start.

Use the provided server (or any host that sends the same COOP/COEP headers
over https): WebAssembly threads need a cross-origin-isolated page.

## Local testing with embedded files (never publish)

For local testing only, `--embed-private-rom` copies the ROM and BIOS into the
bundle, in `<project>/web-PRIVATE` with `PRIVATE-` file names, so the page
starts without asking:

```sh
bash packaging/web/build_web.sh --embed-private-rom /path/to/game-recomp \
  build/generated_bios /path/to/game.gba bios/gba_bios.bin /path/to/game.toml
```

That directory contains copyrighted files. Never upload, deploy, share or
commit it. Add `--dev` to allow `?args=` / `?env=` URL parameters when the page
is opened from localhost (used by the browser acceptance tests).
