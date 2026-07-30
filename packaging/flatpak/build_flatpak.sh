#!/usr/bin/env bash
# build_flatpak.sh — generate a Flatpak manifest for a gbarecomp game and,
# when flatpak-builder is present, build and install it.
#
#   gbarecomp/packaging/flatpak/build_flatpak.sh \
#       --target BoktaiRecomp --name "Boktai" --id tech.recomp.BoktaiRecomp
#
# On a Steam Deck, run it in Desktop Mode. The Deck's root filesystem is
# immutable, which is exactly why this path uses Flatpak: the runtime supplies
# SDL2 and the toolchain, so nothing has to be installed into the OS.
#
# --generate-only writes the manifest and support files without building, which
# is what CI does (a build needs the recompiled sources, which need the ROM).

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TARGET=""
APP_NAME=""
APP_ID=""
SOURCE_DIR="$(pwd)"
SUMMARY=""
GENERATE_ONLY=0
INSTALL=1

usage() {
    cat <<'USAGE'
build_flatpak.sh --target <cmake-target> [options]

  --target <name>     CMake target / command name (required)
  --name <name>       display name (default: the target name)
  --id <app-id>       Flatpak app id (default: tech.recomp.<target>)
  --summary <text>    one-line description for the metainfo
  --source <dir>      game repo root to build from (default: cwd)
  --generate-only     write the manifest + support files, do not build
  --no-install        build but do not install into the user remote
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --target)        TARGET="$2"; shift 2 ;;
        --name)          APP_NAME="$2"; shift 2 ;;
        --id)            APP_ID="$2"; shift 2 ;;
        --summary)       SUMMARY="$2"; shift 2 ;;
        --source)        SOURCE_DIR="$2"; shift 2 ;;
        --generate-only) GENERATE_ONLY=1; shift ;;
        --no-install)    INSTALL=0; shift ;;
        -h|--help)       usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

[ -n "$TARGET" ] || { echo "--target is required" >&2; usage; exit 2; }
APP_NAME="${APP_NAME:-$TARGET}"
APP_ID="${APP_ID:-tech.recomp.$TARGET}"
SUMMARY="${SUMMARY:-$APP_NAME, statically recompiled}"
SOURCE_DIR="$(cd "$SOURCE_DIR" && pwd)"

OUT="$SOURCE_DIR/flatpak-build"
mkdir -p "$OUT"

subst() {
    sed -e "s|@APP_ID@|$APP_ID|g" \
        -e "s|@APP_NAME@|$APP_NAME|g" \
        -e "s|@TARGET@|$TARGET|g" \
        -e "s|@SUMMARY@|$SUMMARY|g" \
        -e "s|@SOURCE_DIR@|$SOURCE_DIR|g" \
        -e "s|@SUPPORT_DIR@|$OUT|g"
}

# Build the source `skip:` list from what this tree actually contains, so the
# template stays game-agnostic (layouts differ: variants/<name>/roms in the
# multi-variant repos, a flat roms/ elsewhere).
echo "==> skip list"
SKIP_FILE="$(mktemp)"
{
    # Any directory holding cartridge dumps, plus any BIOS image.
    ( cd "$SOURCE_DIR" && find . -type d -name roms -not -path '*/.git/*' \
        -printf '          - %P\n' 2>/dev/null ) || true
    ( cd "$SOURCE_DIR" && find . -type f \( -name '*.gba' -o -name '*.agb' \
        -o -name 'gba_bios.bin' \) -not -path '*/.git/*' \
        -printf '          - %P\n' 2>/dev/null ) || true
    # Build output and caches: large, host-specific, and never an input.
    for d in build build-ui build-release build-sub release-stage \
             recomp_cache flatpak-build .flatpak-builder .git; do
        if [ -e "$SOURCE_DIR/$d" ]; then echo "          - $d"; fi
    done
    # The group's exit status is the last command's, and a false `[ -e ]` would
    # make the pipeline fail under `set -o pipefail` and abort the script.
    true
} | sort -u > "$SKIP_FILE"
sed 's/^          - /    /' "$SKIP_FILE" | sed 's/^/    skipping: /'

echo "==> manifest"
subst < "$HERE/manifest.template.yml" | \
    awk -v skipfile="$SKIP_FILE" '
        /^@SKIP@$/ {
            print "        skip:"
            while ((getline line < skipfile) > 0) print line
            next
        }
        { print }
    ' > "$OUT/$APP_ID.yml"
rm -f "$SKIP_FILE"

echo "==> desktop entry"
# Categories=Game;Emulator; is what Steam's "Add a Non-Steam Game" and the Deck's
# application grid key off.
cat > "$OUT/$APP_ID.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=$APP_NAME
Comment=$SUMMARY
Exec=$TARGET
Icon=$APP_ID
Categories=Game;Emulator;
Terminal=false
DESKTOP

echo "==> metainfo"
cat > "$OUT/$APP_ID.metainfo.xml" <<METAINFO
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>$APP_ID</id>
  <name>$APP_NAME</name>
  <summary>$SUMMARY</summary>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>LicenseRef-proprietary</project_license>
  <description>
    <p>
      A statically recompiled Game Boy Advance title built with gbarecomp. This
      package contains no game data: you supply your own legally-dumped ROM and
      BIOS, which the launcher prompts for on first run.
    </p>
  </description>
  <launchable type="desktop-id">$APP_ID.desktop</launchable>
  <content_rating type="oars-1.1"/>
</component>
METAINFO

# A placeholder icon keeps the manifest's install step honest; a game repo should
# drop its own 256x256 PNG in beside the manifest to replace it.
if [ ! -f "$OUT/$APP_ID.png" ]; then
    if [ -f "$SOURCE_DIR/packaging/icon.png" ]; then
        cp "$SOURCE_DIR/packaging/icon.png" "$OUT/$APP_ID.png"
        echo "==> icon (from packaging/icon.png)"
    else
        # 1x1 transparent PNG. Flatpak requires the file to exist; replace it.
        printf '\211PNG\r\n\032\n\0\0\0\rIHDR\0\0\0\1\0\0\0\1\10\6\0\0\0\37\25\304\211\0\0\0\nIDATx\234c\0\1\0\0\5\0\1\r\n\055\264\0\0\0\0IEND\256B`\202' \
            > "$OUT/$APP_ID.png"
        echo "==> icon: placeholder written (add packaging/icon.png for a real one)"
    fi
fi

echo
echo "Manifest and support files in $OUT"

if [ "$GENERATE_ONLY" = 1 ]; then
    echo "(--generate-only: stopping before the build)"
    exit 0
fi

# Resolve a builder. On SteamOS there is no native flatpak-builder and none can
# be installed — the root filesystem is immutable — so the flatpak-packaged
# org.flatpak.Builder is the only option there, and is what the Deck actually
# has. Prefer a native binary when one exists (ordinary distros).
if command -v flatpak-builder >/dev/null 2>&1; then
    BUILDER=(flatpak-builder)
elif flatpak info org.flatpak.Builder >/dev/null 2>&1; then
    # --filesystem=host so the sandboxed builder can read the source tree and
    # write the output dir; without it every `type: dir` source fails to resolve.
    BUILDER=(flatpak run --filesystem=host --share=network org.flatpak.Builder)
    echo "==> using flatpak-packaged org.flatpak.Builder"
else
    cat >&2 <<'MSG'

No flatpak-builder available. On a Steam Deck, in Desktop Mode:

    flatpak install -y --user flathub org.flatpak.Builder
    flatpak install -y --user flathub org.freedesktop.Platform/x86_64/23.08 \
                                      org.freedesktop.Sdk/x86_64/23.08

then re-run this script.
MSG
    exit 1
fi

# The recompiled sources are what make this buildable at all; fail early and
# clearly rather than deep inside the sandbox.
if ! find "$SOURCE_DIR" -path '*/generated/*' \( -name '*.c' -o -name '*.cpp' \) \
        -print -quit | grep -q .; then
    echo "no recompiled sources found under $SOURCE_DIR" >&2
    echo "run gba_recompile over your ROM first — generated/ is not committed" >&2
    exit 1
fi

ARGS=(--force-clean --user)
[ "$INSTALL" = 1 ] && ARGS+=(--install)

echo "==> ${BUILDER[*]}"
"${BUILDER[@]}" "${ARGS[@]}" "$OUT/build" "$OUT/$APP_ID.yml"

echo
echo "Done. Launch with:  flatpak run $APP_ID"
echo
echo "To put it in Steam's library (Deck Gaming Mode): in Desktop Mode, Steam >"
echo "Games > Add a Non-Steam Game, pick $APP_NAME, then switch to Gaming Mode."
