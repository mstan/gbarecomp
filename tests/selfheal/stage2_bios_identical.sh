#!/usr/bin/env bash
# stage2_bios_identical.sh — Stage-2 self-heal GFP1 byte-identity gate.
#
# Proves that a runtime-recompiled (healed-to-native) BIOS function executes
# instruction-for-instruction identically to the fully-static corpus, using the
# 0x00000BA4 exclude fixture (bios/gba_bios_excltest.toml). Runs four passes and
# asserts their per-instruction fingerprint rings (GFP1) are byte-identical:
#
#   A  (static)     canonical BIOS, recompile off            -> gfp_static.bin
#   B1 (bridge)     0xBA4 excluded, recompile off            -> gfp_bridge.bin
#   B2 (heal cold)  0xBA4 excluded, recompile on, cold cache -> gfp_heal_cold.bin
#   B3 (heal warm)  0xBA4 excluded, recompile on, warm cache -> gfp_heal_warm.bin
#
#   static == bridge      : Stage-1 bridge regression guard
#   static == heal_cold   : cold pass still matches (bridge fallback this run)
#   static == heal_warm   : the NATIVE DLL body matches the static corpus
#   heal_cold == heal_warm: cache transparency
#
# This is heavy (two MC relinks) and needs the (gitignored) ROM + BIOS + a built
# MinishCapRecomp, so it is opt-in. Configure via env:
#   GBARECOMP_ROOT   gbarecomp worktree (default: this script's ../..)
#   MC_DIR           MinishCapRecomp project dir (REQUIRED)
#   MC_BUILD         MC build dir under MC_DIR  (default: build-selfheal)
#   ROM              path to the cart ROM        (default: $MC_DIR/roms/minishcap_usa.gba)
#   BIOS             path to gba_bios.bin        (default: $GBARECOMP_ROOT/bios/gba_bios.bin)
#   NINJA / CMAKE_BIN/ GXX paths default to msys2 mingw64.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${GBARECOMP_ROOT:-$(cd "$here/../.." && pwd)}"
: "${MC_DIR:?set MC_DIR to the MinishCapRecomp project dir}"
MC_BUILD="${MC_BUILD:-$MC_DIR/build-selfheal}"
ROM="${ROM:-$MC_DIR/roms/minishcap_usa.gba}"
BIOS="${BIOS:-$ROOT/bios/gba_bios.bin}"
NINJA="${NINJA:-/c/msys64/mingw64/bin/ninja.exe}"
TOOL="$ROOT/build/gba_recompile.exe"
MC_EXE="$MC_BUILD/MinishCapRecomp.exe"
FRAMES="${FRAMES:-3}"

export PATH="/c/msys64/mingw64/bin:$PATH"

for f in "$ROM" "$BIOS" "$TOOL" "$MC_EXE"; do
    [ -e "$f" ] || { echo "SKIP: missing $f"; exit 77; }  # 77 = ctest "skipped"
done

work="$(mktemp -d)"
canonical_cfg="$ROOT/bios/gba_bios.toml"
exclude_cfg="$ROOT/bios/gba_bios_excltest.toml"

regen() { "$TOOL" --bios "$BIOS" --config "$1" >/dev/null; }
relink() { "$NINJA" -C "$MC_BUILD" MinishCapRecomp >/dev/null; }
run() {  # run <out.bin> [extra env assignments...]
    local out="$1"; shift
    taskkill //F //IM MinishCapRecomp.exe >/dev/null 2>&1 || true
    env "$@" GBARECOMP_INSN_TRACE=1 GBARECOMP_FP_SAVE="$out" \
        "$MC_EXE" --bios "$BIOS" --rom "$ROM" --frames "$FRAMES" --no-window \
        >/dev/null 2>&1
    [ -s "$out" ] || { echo "FAIL: no fingerprint ring at $out"; exit 1; }
}

# Always restore the canonical BIOS corpus, even on failure.
cleanup() { regen "$canonical_cfg" >/dev/null 2>&1 || true; relink >/dev/null 2>&1 || true; rm -rf "$work"; }
trap cleanup EXIT

echo "== A: static (canonical BIOS, recompile off) =="
regen "$canonical_cfg"; relink
run "$work/static.bin"

echo "== B*: 0xBA4 excluded =="
regen "$exclude_cfg"; relink
rm -rf "$MC_DIR/recomp_cache"
run "$work/bridge.bin"                                  # B1 recompile off
rm -rf "$MC_DIR/recomp_cache"
( cd "$MC_DIR" && run "$work/heal_cold.bin" GBARECOMP_SELFHEAL_RECOMPILE=1 )  # B2 cold
( cd "$MC_DIR" && run "$work/heal_warm.bin" GBARECOMP_SELFHEAL_RECOMPILE=1 )  # B3 warm

echo "== cmp (all four must be byte-identical) =="
fail=0
for f in bridge heal_cold heal_warm; do
    if cmp -s "$work/static.bin" "$work/$f.bin"; then
        echo "  static == $f : IDENTICAL"
    else
        echo "  static != $f : DIVERGED"; fail=1
    fi
done
[ "$fail" = 0 ] && echo "PASS: Stage-2 heal is GFP1 byte-identical to static" \
                || { echo "FAIL"; exit 1; }
