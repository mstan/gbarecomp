#!/usr/bin/env bash

set -euo pipefail

PACKAGING_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
FLATPAK_ROOT="$PACKAGING_ROOT/flatpak"

for script in \
    "$PACKAGING_ROOT/package_macos.sh" \
    "$PACKAGING_ROOT/package_linux.sh" \
    "$FLATPAK_ROOT/build_flatpak.sh"; do
    bash -n "$script"
done

PYTHON="${PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || {
    echo "Python 3 is required for YAML and XML validation" >&2
    exit 1
}
command -v desktop-file-validate >/dev/null 2>&1 || {
    echo "desktop-file-validate is required (desktop-file-utils)" >&2
    exit 1
}

TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT
SOURCE="$TMP/source & team's tree"
mkdir -p -- \
    "$SOURCE/build-pr-6" \
    "$SOURCE/build-cosim" \
    "$SOURCE/nested/roms"
: > "$SOURCE/nested/roms/decoy.gba"
: > "$SOURCE/gba_bios.bin"

APP_ID="tech.recomp.FireRedLeafGreen"
APP_NAME="FireRed & LeafGreen"
SUMMARY='FireRed & LeafGreen <generation test> "quoted"'

"$FLATPAK_ROOT/build_flatpak.sh" \
    --target FireRedRecomp \
    --name "$APP_NAME" \
    --id "$APP_ID" \
    --summary "$SUMMARY" \
    --source "$SOURCE" \
    --generate-only

OUT="$SOURCE/flatpak-build"
MANIFEST="$OUT/$APP_ID.yml"
METAINFO="$OUT/$APP_ID.metainfo.xml"
DESKTOP="$OUT/$APP_ID.desktop"

"$PYTHON" - "$MANIFEST" "$METAINFO" "$FLATPAK_ROOT/manifest.builder.template.yml" <<'PY'
import pathlib
import sys
import xml.etree.ElementTree as ET

try:
    import yaml
except ImportError as exc:
    raise SystemExit("PyYAML is required for packaging generation tests") from exc

manifest_path, metainfo_path, builder_template_path = map(pathlib.Path, sys.argv[1:])
manifest_text = manifest_path.read_text(encoding="utf-8")
assert "@APP_" not in manifest_text and "@SOURCE_DIR@" not in manifest_text
manifest = yaml.safe_load(manifest_text)
assert manifest["runtime-version"] == "25.08"

source = manifest["modules"][-1]["sources"][0]
skips = set(source["skip"])
for expected in {
    "build-pr-6",
    "build-cosim",
    "nested/roms",
    "nested/roms/decoy.gba",
    "gba_bios.bin",
    "flatpak-build",
}:
    assert expected in skips, f"missing skip entry: {expected}"

root = ET.parse(metainfo_path).getroot()
assert root.findtext("name") == "FireRed & LeafGreen"
assert root.findtext("summary") == 'FireRed & LeafGreen <generation test> "quoted"'

# The builder template is consumed by game repositories. Render its structural
# placeholders here so its YAML and runtime pin are checked alongside the game
# manifest even though this generic script does not build a specific builder.
builder_text = builder_template_path.read_text(encoding="utf-8")
for key, value in {
    "@APP_ID@": "tech.recomp.TestBuilder",
    "@TARGET@": "TestBuilder",
    "@SOURCE_DIR@": "/tmp/source & tree",
    "@SUPPORT_DIR@": "/tmp/support & tree",
}.items():
    builder_text = builder_text.replace(key, value)
builder_text = builder_text.replace("@SKIP@", "        skip: []")
builder = yaml.safe_load(builder_text)
assert builder["runtime-version"] == "25.08"
assert builder["sdk-version"] == "25.08"
PY

desktop-file-validate "$DESKTOP"

# A path-like distribution name must be rejected before either release script
# can configure, stage, or remove anything.
if "$PACKAGING_ROOT/package_linux.sh" \
        --target TestTarget --name '../outside' --root "$SOURCE" >/dev/null 2>&1; then
    echo "package_linux.sh accepted an unsafe --name" >&2
    exit 1
fi

# Exercise the shared release-stage path logic through the Linux implementation.
# Invoking it from the parent with a relative --root previously produced
# <root>/<root>/build-release and never reached a valid staged artifact.
RELATIVE_SOURCE="$TMP/relative-root"
mkdir -p -- "$RELATIVE_SOURCE"
cat > "$RELATIVE_SOURCE/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(packaging_path_test C)
add_executable(Tiny main.c)
CMAKE
cat > "$RELATIVE_SOURCE/main.c" <<'C'
int main(void) { return 0; }
C
(
    cd -- "$TMP"
    "$PACKAGING_ROOT/package_linux.sh" \
        --target Tiny --name Tiny --root relative-root
)
ARCH="$(uname -m)"
test -x "$RELATIVE_SOURCE/release-stage/Tiny-linux-$ARCH/Tiny"
test -f "$RELATIVE_SOURCE/release-stage/Tiny-linux-$ARCH.tar.gz"

echo "packaging generation checks passed"
