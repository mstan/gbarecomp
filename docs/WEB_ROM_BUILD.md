# Build and Run a ROM in the Browser

From the project root, activate Emscripten and build the native recompiler:

```sh
source ~/emsdk/emsdk_env.sh

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGBARECOMP_COMPILER_CACHE=OFF
cmake --build build --target gba_recompile -- -j10

build/gba_recompile --bios bios/gba_bios.bin --config bios/gba_bios.toml \
  --out build/generated_bios
```

Export the ROM and build the browser bundle:

```sh
GBARECOMP_CORE="$PWD/build/gba_recompile" python3 tools/cli.py build \
  --rom /path/to/game.gba \
  --output /path/to/game-recomp \
  --config /path/to/game.toml \
  --force

bash packaging/web/build_web.sh /path/to/game-recomp \
  build/generated_bios /path/to/game.gba bios/gba_bios.bin
```

Serve the built files on port 8080:

```sh
python3 packaging/web/serve.py /path/to/game-recomp/web 8080
```

Open http://127.0.0.1:8080/ in a browser.

Use the provided server because it sends the headers required by WebAssembly
threads.
