# hqx shared library

Original source: https://github.com/grom358/hqx
Commit: `a1c7d415f4bb7b947c4c2e47faf220b0148f4bac`.

The upstream C files and headers are unchanged and remain licensed under
LGPL-2.1-or-later. The accompanying CMake build file uses the same license.
See COPYING and AUTHORS. This directory's CMakeLists.txt
builds the original library as a separate, replaceable shared library. The
host loads it at runtime and uses HQ2x; HQ3x/HQ4x remain available in its ABI.

To rebuild with CMake and a C compiler:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
```

Use the game's CPU architecture and calling convention. On Windows, use the
same compiler family (MinGW or MSVC) for 32-bit builds. Replace
`gbarecomp-hqx.dll`, `gbarecomp-hqx.so` or `gbarecomp-hqx.dylib` beside the game
with the corresponding result. On multi-configuration generators the result
is in `build/Release`. The host permits replacement of this library and
debugging of those modifications. This source directory is supplied with
game packages that include it.
