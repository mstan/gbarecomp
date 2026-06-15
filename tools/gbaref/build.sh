#!/usr/bin/env bash
# Build gbaref.exe with the project's msys2 mingw64 toolchain (same as the rest
# of gbarecomp; NOT MSVC). SDL2 comes from msys2 mingw64.
#
#   PATH=/c/msys64/mingw64/bin:$PATH ./build.sh
#
# At runtime supply a libretro GBA core DLL (e.g. mgba_libretro.dll) as argv[1];
# the core is licensed separately and is intentionally not committed here.
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw64/bin:$PATH"
c++ -std=c++17 -O2 -DSDL_MAIN_HANDLED \
    frontend.cpp \
    $(pkg-config --cflags sdl2) \
    -o gbaref.exe \
    $(pkg-config --libs sdl2)
echo "built: $(pwd)/gbaref.exe"
