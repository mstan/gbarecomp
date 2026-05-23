#!/usr/bin/env bash
# oracle/setup-mgba.sh — clone mGBA at a pinned tag, apply our patches,
# and build libmgba. Idempotent (skips clone/apply when already done).
#
# Usage: from gbarecomp/ root, run `bash oracle/setup-mgba.sh`.
# Requires MSYS2 MinGW64 (the build is driven via cmake from PowerShell
# afterwards). See oracle/README.md for the full flow.

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MGBA_DIR="$REPO_ROOT/third_party/mgba"
MGBA_TAG="0.10.5"
PATCHES_DIR="$REPO_ROOT/oracle/patches"

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
if ! grep -q '^if(MSYS)' CMakeLists.txt; then
    echo "==> Applying 0001-msys2-as-windows.patch"
    git apply "$PATCHES_DIR/0001-msys2-as-windows.patch"
fi
if grep -q '^#ifdef USE_PTHREADS$' src/core/thread.c; then
    echo "==> Applying 0002-windows-no-sigtrap.patch"
    git apply "$PATCHES_DIR/0002-windows-no-sigtrap.patch"
fi

echo "==> mGBA source ready. Now from PowerShell:"
echo "    cd third_party\\mgba"
echo "    cmake -B build -S . -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \\"
echo "        -DLIBMGBA_ONLY=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF \\"
echo "        -DUSE_FFMPEG=OFF -DUSE_LIBZIP=OFF -DUSE_LZMA=OFF \\"
echo "        -DUSE_EPOXY=OFF -DUSE_DEBUGGERS=OFF \\"
echo "        -DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF"
echo "    cmake --build build -j 8"
echo "Then back at gbarecomp/ root:"
echo "    cmake -B build -S . -DGBARECOMP_BUILD_ORACLE=ON"
echo "    cmake --build build --target gbarecomp_oracle"
