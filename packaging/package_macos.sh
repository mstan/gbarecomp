#!/usr/bin/env bash
# package_macos.sh — build a game target into a self-contained, portable
# directory on macOS, plus a .tar.gz of it.
#
# Game-agnostic: every gbarecomp game exe is a plain SDL2 program, so the same
# steps work for any of them. Run from a game repo root:
#
#   gbarecomp/packaging/package_macos.sh --target BoktaiRecomp \
#       --name Boktai -DGBAGAME_RECOMP_UI=ON
#
# Output: <root>/release-stage/<name>-macos-<arch>/  (+ .tar.gz)
#
# ---------------------------------------------------------------------------
# Why a flat directory and not a .app bundle
#
# A .app was tried first and does not work: launching
# Contents/MacOS/<exe> makes the kernel validate the whole bundle, and the
# process is SIGKILLed (exit 137) before it prints a byte. The cause is
# structural, not a signing mistake — the launcher resolves its assets directory
# as the executable's own directory, so assets/ has to sit in Contents/MacOS,
# and codesign reads everything under Contents/MacOS as code:
#
#     code object is not signed at all
#     In subcomponent: .../Contents/MacOS/assets/fonts/OpenMoji-black-glyf.ttf
#
# so the bundle can never carry a valid signature while the assets are where the
# runtime needs them. A flat directory is not bundle-validated, runs signed or
# unsigned, and was verified booting the real ROM for 30 frames.
#
# Why not GBARECOMP_STATIC_RELEASE: it pins C:/msys64/mingw64/lib/libSDL2.a and
# links Windows system libs, so it is MSYS2-only by construction. macOS has no
# static libSystem either, so a fully static binary is not a supported
# configuration here.
# ---------------------------------------------------------------------------

set -euo pipefail

TARGET=""; APP_NAME=""; BUILD_DIR="build-release"; ROOT="$(pwd)"
CONFIGURE_ARGS=()

usage() {
    cat <<'USAGE'
package_macos.sh --target <cmake-target> [options]

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
[ "$(uname -s)" = "Darwin" ] || { echo "package_macos.sh only runs on macOS" >&2; exit 1; }

cd "$ROOT"
BUILD_PATH="$ROOT/$BUILD_DIR"
ARCH="$(uname -m)"
OUT="$ROOT/release-stage/$APP_NAME-macos-$ARCH"

echo "==> configure ($BUILD_DIR)"
cmake -S "$ROOT" -B "$BUILD_PATH" -DCMAKE_BUILD_TYPE=Release \
    "${CONFIGURE_ARGS[@]+"${CONFIGURE_ARGS[@]}"}"

echo "==> build $TARGET"
cmake --build "$BUILD_PATH" --target "$TARGET" -j"$(sysctl -n hw.ncpu)"

EXE="$BUILD_PATH/$TARGET"
[ -x "$EXE" ] || { echo "built executable not found: $EXE" >&2; exit 1; }

echo "==> stage $OUT"
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$EXE" "$OUT/$TARGET"

# recomp-ui stages launcher assets next to the built exe; the seam resolves them
# relative to the executable, so they travel beside it.
[ -d "$BUILD_PATH/assets" ] && cp -R "$BUILD_PATH/assets" "$OUT/assets" &&
    echo "    + launcher assets"

# ---- copy every non-system dylib next to the exe --------------------------
echo "==> collect dylibs"
relocate() {
    local binary="$1" dep base
    otool -L "$binary" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        case "$dep" in /usr/lib/*|/System/*|@*) continue ;; esac
        base="$(basename "$dep")"
        if [ ! -f "$OUT/$base" ]; then
            cp -L "$dep" "$OUT/$base"; chmod u+w "$OUT/$base"
            install_name_tool -id "@executable_path/$base" "$OUT/$base" 2>/dev/null
            echo "    + $base"
            relocate "$OUT/$base"
        fi
        install_name_tool -change "$dep" "@executable_path/$base" "$binary" 2>/dev/null
    done
}
relocate "$OUT/$TARGET"

# ---- satisfy dlopen'd dependencies ---------------------------------------
# otool -L cannot see a library that is dlopen'd rather than linked, and missing
# one is not a link error — it is a hard abort at runtime with no output.
#
# sdl2-compat is exactly this case and is what Homebrew's `sdl2` now installs on
# many systems: its libSDL2 is a shim that dlopens SDL3, probing
# @loader_path/libSDL3.dylib first. Copying SDL3 in beside it satisfies that
# probe. Without it the game dies with "failed to load SDL3".
echo "==> resolve dlopen'd libraries"
for lib in "$OUT"/*.dylib; do
    [ -f "$lib" ] || continue
    strings "$lib" | grep -oE '@loader_path/lib[A-Za-z0-9._+-]*\.dylib' |
        sed 's|@loader_path/||' | sort -u | while read -r want; do
        [ -f "$OUT/$want" ] && continue
        found=""
        for p in "$(brew --prefix 2>/dev/null)/lib" /opt/homebrew/lib \
                 /usr/local/lib /opt/homebrew/opt/*/lib; do
            [ -f "$p/$want" ] && { found="$p/$want"; break; }
        done
        if [ -n "$found" ]; then
            cp -L "$found" "$OUT/$want"; chmod u+w "$OUT/$want"
            echo "    + $want (dlopen'd by $(basename "$lib"))"
        else
            echo "    WARNING: $(basename "$lib") dlopens $want, not found" >&2
            echo "             the game will abort at startup without it" >&2
        fi
    done
done

# install_name_tool invalidates the linker's ad-hoc signature and an
# invalidly-signed arm64 binary is killed on launch, so re-sign each file. Safe
# here precisely because nothing is inside a .app: codesign sees plain Mach-Os.
echo "==> ad-hoc codesign"
if command -v codesign >/dev/null 2>&1; then
    for f in "$OUT"/*.dylib "$OUT/$TARGET"; do
        [ -f "$f" ] || continue
        codesign --remove-signature "$f" 2>/dev/null || true
        codesign --force --sign - "$f" 2>/dev/null &&
            echo "    signed $(basename "$f")"
    done
fi

cat > "$OUT/README.txt" <<TXT
$APP_NAME — macOS ($ARCH)

Run ./$TARGET from this directory. Everything it needs is here; nothing is
installed system-wide.

You must supply your own legally-dumped GBA BIOS and cartridge ROM. The
launcher prompts for both on first run and caches the paths beside this file.

If macOS refuses to open it after a download:
    xattr -dr com.apple.quarantine "$(basename "$OUT")"
TXT

echo "==> verify"
LEFT="$(otool -L "$OUT/$TARGET" | tail -n +2 | awk '{print $1}' |
        grep -vE '^(/usr/lib|/System|@executable_path)' || true)"
[ -n "$LEFT" ] && { echo "    WARNING: unrelocated deps:" >&2; printf '      %s\n' $LEFT >&2; } ||
    echo "    every dependency is a system lib or beside the executable"

# Smoke test: --help exercises startup, dynamic linking and every dlopen the
# runtime performs before it needs a ROM. A broken payload SIGKILLs here.
printf "    smoke test: "
if ( cd "$OUT" && ./"$TARGET" --help >/dev/null 2>&1 ); then
    echo "startup OK"
else
    echo "FAILED (exit $?) — the payload is incomplete" >&2
    exit 1
fi

echo "==> tarball"
( cd "$ROOT/release-stage" && tar -czf "$APP_NAME-macos-$ARCH.tar.gz" \
    "$APP_NAME-macos-$ARCH" )
echo "    release-stage/$APP_NAME-macos-$ARCH.tar.gz"

echo
echo "Built $OUT"
echo "Run:   (cd \"$OUT\" && ./$TARGET)"
