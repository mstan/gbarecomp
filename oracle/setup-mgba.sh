#!/usr/bin/env bash
# oracle/setup-mgba.sh — clone mGBA at a pinned tag, apply our patches,
# and build libmgba. Idempotent (skips clone/apply when already done).
#
# Usage: from gbarecomp/ root, run `bash oracle/setup-mgba.sh`.
# On Windows this expects MSYS2 MinGW64 (the build is then driven via cmake
# from PowerShell). macOS/Linux need no extra shell. See oracle/README.md.

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MGBA_DIR="$REPO_ROOT/third_party/mgba"
MGBA_TAG="0.10.5"
PATCHES_DIR="$REPO_ROOT/oracle/patches"

# Both patches in oracle/patches/ are Windows-specific — 0001 makes MSYS look
# like Windows to mGBA's CMake, and 0002 drops a SIGTRAP path that only exists
# there. Applying them off-Windows misconfigures the build, so gate on the host.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) HOST_IS_WINDOWS=1 ;;
    *)                    HOST_IS_WINDOWS=0 ;;
esac

if [ ! -d "$MGBA_DIR/.git" ]; then
    echo "==> Cloning mGBA $MGBA_TAG into $MGBA_DIR"
    git clone --depth 1 --branch "$MGBA_TAG" \
        https://github.com/mgba-emu/mgba.git "$MGBA_DIR"
else
    echo "==> mGBA already cloned at $MGBA_DIR"
fi

# Apply patches if they haven't been applied yet. We detect by checking
# the patched line in the target file — `git apply --check` would also
# work but is noisier.
cd "$MGBA_DIR"
if [ "$HOST_IS_WINDOWS" -eq 1 ]; then
    if ! grep -q '^if(MSYS)' CMakeLists.txt; then
        echo "==> Applying 0001-msys2-as-windows.patch"
        git apply "$PATCHES_DIR/0001-msys2-as-windows.patch"
    fi
    if grep -q '^#ifdef USE_PTHREADS$' src/core/thread.c; then
        echo "==> Applying 0002-windows-no-sigtrap.patch"
        git apply "$PATCHES_DIR/0002-windows-no-sigtrap.patch"
    fi
else
    echo "==> Non-Windows host: skipping the two MSYS/Windows patches"
fi

CMAKE_ARGS="-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DLIBMGBA_ONLY=ON \
-DBUILD_QT=OFF -DBUILD_SDL=OFF -DUSE_FFMPEG=OFF -DUSE_LIBZIP=OFF \
-DUSE_LZMA=OFF -DUSE_EPOXY=OFF -DUSE_DEBUGGERS=OFF -DBUILD_GL=OFF \
-DBUILD_GLES2=OFF -DBUILD_GLES3=OFF"

if [ "$HOST_IS_WINDOWS" -eq 1 ]; then
    echo "==> mGBA source ready. Now from PowerShell:"
    echo "    cd third_party\\mgba"
    echo "    cmake -B build -S . $CMAKE_ARGS"
    echo "    cmake --build build -j 8"
    echo "Then back at gbarecomp/ root:"
    echo "    cmake -B build -S . -DGBARECOMP_BUILD_ORACLE=ON"
    echo "    cmake --build build --target gbarecomp_oracle"
else
    echo "==> Building libmgba"
    cmake -B build -S . $CMAKE_ARGS >/dev/null
    cmake --build build -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    echo "==> libmgba built at $MGBA_DIR/build/libmgba.a"
    echo "Now from gbarecomp/ root:"
    echo "    cmake -B build -S . -DGBARECOMP_BUILD_ORACLE=ON"
    echo "    cmake --build build --target gbarecomp_oracle"
fi
