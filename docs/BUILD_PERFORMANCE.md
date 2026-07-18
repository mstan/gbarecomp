# Build performance

`gba_recompile` is a native C++ program and normally analyzes and emits a full
game in seconds. The expensive stage is the host compiler parsing hundreds of
megabytes of generated C++. A single `recompiled.cpp` prevents parallelism and
has taken 10--22 minutes for Minish Cap on an 8-core i9-9900K.

## Cartridge monoliths are prohibited

Current `gba_recompile` always emits at least two deterministic files named
`recompiled_NNN.cpp`. When `[program].codegen_shards` is omitted, an adaptive
policy targets about 3,000 discovered functions per shard, rounds up to a power
of two, and clamps the result to 2..64. Minish Cap's current 44,584-function
corpus selects 16 shards. An explicit value from 2..256 remains available for
large projects and reproducible tuning.

Generation removes a stale `recompiled.cpp` after the replacement shards are
committed. `gbarecomp_add_runtime_target(... GAME_GENERATED_DIR <dir>)` also
refuses to configure while a stale cartridge monolith exists.

The BIOS is only 16 KiB and intentionally remains one translation unit.

## Fast Windows build

LLVM's Clang frontend can produce GNU-ABI MinGW objects that link with the
existing runtime and libraries. Install LLVM for Windows and MSYS2 MinGW64,
then configure a game with Ninja and the supplied toolchain:

```powershell
C:\msys64\mingw64\bin\cmake.exe -S . -B build-clang -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_TOOLCHAIN_FILE=../gbarecomp/cmake/toolchains/clang-mingw.cmake"
C:\msys64\mingw64\bin\cmake.exe --build build-clang --parallel
```

The toolchain defaults to `C:/Program Files/LLVM` and
`C:/msys64/mingw64`; override `GBARECOMP_LLVM_ROOT` or
`GBARECOMP_MINGW_ROOT` when needed.

Compiler caching is automatic when `sccache` or `ccache` is installed. Disable
it with `-DGBARECOMP_COMPILER_CACHE=OFF`, or select a specific executable with
`-DGBARECOMP_COMPILER_CACHE=<path-or-name>`. An explicit
`CMAKE_CXX_COMPILER_LAUNCHER` always wins.

## Measured acceptance baseline

On the i9-9900K audit machine, the 201.7 MiB Minish Cap corpus measured:

| Build shape | Generated-code compile time |
|-------------|-----------------------------|
| One GCC translation unit | 10m34s--21m37s (historical Ninja log) |
| 16 GCC shards, 8 jobs | 1m59.6s |
| 16 Clang shards, 8 jobs | 55.9s |
| Full Minish Cap Clang build and link | 1m3.8s |

The full Clang build produced the existing Windows executable successfully.
An unchanged cached shard fell from 24.7 seconds to 0.037 seconds. The project
acceptance target is a reliable low-single-digit-minute clean build, not a
fragile benchmark-only shortcut.
