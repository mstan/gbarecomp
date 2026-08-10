#!/usr/bin/env bash
# package_linux.sh — build a game target into a self-contained, portable
# directory on Linux, plus a .tar.gz of it.
#
#   gbarecomp/packaging/package_linux.sh --target BoktaiRecomp --name Boktai \
#       -DGBAGAME_RECOMP_UI=ON
#
# Output: <root>/release-stage/<name>-linux-<arch>/  (+ .tar.gz)
#
# This is the counterpart to package_macos.sh and works the same way: copy every
# non-system shared library next to the executable and make the binary look for
# them there. On Linux that means an $ORIGIN rpath rather than @executable_path.
#
# For a Steam Deck, prefer packaging/flatpak/ instead — SteamOS has an immutable
# root, so a Flatpak (which brings its own runtime) is a better fit than a tarball
# that depends on whatever the host happens to ship. This script is for ordinary
# distributions, and for CI artifacts.
#
# GBARECOMP_STATIC_RELEASE is deliberately unused: it pins
# C:/msys64/mingw64/lib/libSDL2.a and links Windows system libs, so it is
# MSYS2-only by construction.

set -euo pipefail

TARGET=""; APP_NAME=""; BUILD_DIR="build-release"; ROOT="$(pwd)"
CONFIGURE_ARGS=()

usage() {
    cat <<'USAGE'
package_linux.sh --target <cmake-target> [options]

  --target <name>      CMake target to build (required)
  --name <name>        distribution name (default: the target name)
  --build-dir <dir>    build directory (default: build-release)
  --root <dir>         game repo root (default: cwd)
  -D<var>=<value>      passed through to the cmake configure step
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --target)    TARGET="$2"; shift 2 ;;
        --name)      APP_NAME="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --root)      ROOT="$2"; shift 2 ;;
        -D*)         CONFIGURE_ARGS+=("$1"); shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

[ -n "$TARGET" ] || { echo "--target is required" >&2; usage; exit 2; }
APP_NAME="${APP_NAME:-$TARGET}"

case "$APP_NAME" in
    ""|[.-]*|*/*|*\\*|*$'\n'*|*$'\r'*)
        echo "--name must be a visible filename component without slashes" >&2
        exit 2
        ;;
esac
[ "$(uname -s)" = "Linux" ] || { echo "package_linux.sh only runs on Linux" >&2; exit 1; }

ROOT="$(cd -- "$ROOT" && pwd -P)"
cd -- "$ROOT"
case "$BUILD_DIR" in
    /*) BUILD_PATH="$BUILD_DIR" ;;
    *)  BUILD_PATH="$ROOT/$BUILD_DIR" ;;
esac
ARCH="$(uname -m)"
mkdir -p -- "$ROOT/release-stage"
RELEASE_STAGE="$(cd -- "$ROOT/release-stage" && pwd -P)"
OUT="$RELEASE_STAGE/$APP_NAME-linux-$ARCH"
case "$OUT" in
    "$RELEASE_STAGE"/*) ;;
    *) echo "refusing output outside release-stage: $OUT" >&2; exit 2 ;;
esac

echo "==> configure ($BUILD_DIR)"
# $ORIGIN makes the executable search its own directory for the bundled .so
# files. It must be escaped from CMake AND from the shell, hence the quoting.
cmake -S "$ROOT" -B "$BUILD_PATH" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DCMAKE_INSTALL_RPATH='$ORIGIN' \
    "${CONFIGURE_ARGS[@]+"${CONFIGURE_ARGS[@]}"}"

echo "==> build $TARGET"
cmake --build "$BUILD_PATH" --target "$TARGET" -j"$(nproc)"

EXE="$BUILD_PATH/$TARGET"
[ -x "$EXE" ] || { echo "built executable not found: $EXE" >&2; exit 1; }

echo "==> stage $OUT"
rm -rf -- "$OUT"; mkdir -p -- "$OUT"
cp "$EXE" "$OUT/$TARGET"

[ -d "$BUILD_PATH/assets" ] && cp -R "$BUILD_PATH/assets" "$OUT/assets" &&
    echo "    + launcher assets"

# ---- copy every non-system shared object next to the exe ------------------
# Deliberately NOT everything ldd reports: glibc, libstdc++, libm, the dynamic
# loader and the graphics stack must come from the host, or the binary breaks on
# any machine with a different kernel/driver/glibc than the build box. Bundling
# libGL in particular breaks hardware acceleration.
echo "==> collect shared libraries"
KEEP_OUT='^(linux-vdso|ld-linux|libc|libm|libdl|libpthread|librt|libstdc\+\+|libgcc_s|libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libX11|libxcb|libwayland|libdrm|libgbm|libasound|libpulse)'
ldd "$OUT/$TARGET" | awk '/=> \//{print $3}' | sort -u | while read -r so; do
    base="$(basename "$so")"
    if echo "$base" | grep -qE "$KEEP_OUT"; then continue; fi
    [ -f "$OUT/$base" ] && continue
    cp -L "$so" "$OUT/$base"
    echo "    + $base"
done

# ---- satisfy dlopen'd dependencies ---------------------------------------
# ldd cannot see a library that is dlopen'd rather than linked, and a missing one
# is not a link error -- it is a hard abort at startup with no output. sdl2-compat
# is exactly this case: its libSDL2 is a shim that dlopens SDL3, probing
# $ORIGIN/libSDL3.so.0 first. Copying SDL3 in beside it satisfies that probe.
echo "==> resolve dlopen'd libraries"
for lib in "$OUT"/*.so*; do
    [ -f "$lib" ] || continue
    # `grep` exits 1 when it matches nothing, and MOST libraries dlopen nothing,
    # so under `set -o pipefail` the common case made this pipeline non-zero and
    # `set -e` aborted the whole script right after printing this step's header.
    # Capture with `|| true` and skip empties instead of letting the pipe decide.
    wants="$(strings "$lib" 2>/dev/null |
             grep -oE '\$ORIGIN/lib[A-Za-z0-9._+-]*\.so[0-9.]*' |
             sed 's|\$ORIGIN/||' | sort -u || true)"
    [ -n "$wants" ] || continue
    printf '%s\n' "$wants" | while read -r want; do
        [ -n "$want" ] || continue
        [ -e "$OUT/$want" ] && continue
        found="$(ldconfig -p 2>/dev/null | awk -v w="$want" '$1==w{print $NF; exit}')"
        if [ -n "$found" ] && [ -f "$found" ]; then
            cp -L "$found" "$OUT/$want"
            echo "    + $want (dlopen'd by $(basename "$lib"))"
        else
            echo "    WARNING: $(basename "$lib") dlopens $want, not found" >&2
        fi
    done
done

# The rpath is baked at link time by the configure flags above; verify rather
# than assume, because a silent miss here only shows up on someone else's box.
echo "==> verify rpath"
if command -v patchelf >/dev/null 2>&1; then
    RPATH="$(patchelf --print-rpath "$OUT/$TARGET" 2>/dev/null || true)"
    case "$RPATH" in
        *'$ORIGIN'*) echo "    rpath = $RPATH" ;;
        *) echo "    rpath is '$RPATH', forcing \$ORIGIN"
           patchelf --set-rpath '$ORIGIN' "$OUT/$TARGET" ;;
    esac
else
    echo "    patchelf absent; trusting the link-time rpath"
fi

cat > "$OUT/README.txt" <<TXT
$APP_NAME — Linux ($ARCH)

Run ./$TARGET from this directory. The bundled libraries beside it are found via
an \$ORIGIN rpath; the graphics stack, glibc and libstdc++ come from your system
deliberately, so this needs a reasonably current distribution.

You must supply your own legally-dumped GBA BIOS and cartridge ROM. The launcher
prompts for both on first run and caches the paths beside this file.

On a Steam Deck, prefer the Flatpak build (packaging/flatpak/) — SteamOS has an
immutable root and the Flatpak brings its own runtime.
TXT

# Smoke test: --help exercises startup, dynamic linking and every dlopen the
# runtime performs before it needs a ROM. A broken payload fails here.
printf "==> smoke test: "
if ( cd "$OUT" && ./"$TARGET" --help >/dev/null 2>&1 ); then
    echo "startup OK"
else
    echo "FAILED (exit $?) — the payload is incomplete" >&2
    exit 1
fi

echo "==> tarball"
( cd "$RELEASE_STAGE" && tar -czf "$APP_NAME-linux-$ARCH.tar.gz" -- \
    "$APP_NAME-linux-$ARCH" )
echo "    release-stage/$APP_NAME-linux-$ARCH.tar.gz"

echo
echo "Built $OUT"
