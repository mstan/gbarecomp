#!/usr/bin/env bash
# coverage_sweep.sh — self-heal coverage sweep across gameplay savestates.
#
# The doctrine's definition of "done" is FULLY_STATIC: zero dispatch misses and
# zero heals (PRINCIPLES.md "Coverage honesty is load-bearing"). This drives the
# already-built MinishCapRecomp from every available savestate with the self-heal
# recompiler ON and the headless walk driver, reads each run's
# recomp_coverage.json, and asserts FULLY_STATIC for every scene — surfacing any
# remaining finder gap (and flagging jump-table candidates) for review. A
# NOT_STATIC scene fails the sweep, so it doubles as a coverage regression guard.
#
# Opt-in: needs the gitignored ROM/BIOS/savestates + a built MC. It does NOT
# regen/relink — it measures the CURRENT build. Configure via env:
#   GBARECOMP_ROOT  gbarecomp worktree (default: this script's ../..)
#   MC_DIR          MinishCapRecomp project dir (REQUIRED)
#   MC_BUILD        MC build dir       (default: $MC_DIR/build-selfheal)
#   ROM/BIOS/MC_EXE paths              (sensible defaults)
#   FRAMES          frames per scene   (default: 800)
#   DEMO            GBARECOMP_DEMO_INPUT value (default: walk; "" = no input)
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${GBARECOMP_ROOT:-$(cd "$here/../.." && pwd)}"
: "${MC_DIR:?set MC_DIR to the MinishCapRecomp project dir}"
MC_BUILD="${MC_BUILD:-$MC_DIR/build-selfheal}"
ROM="${ROM:-$MC_DIR/roms/minishcap_usa.gba}"
BIOS="${BIOS:-$ROOT/bios/gba_bios.bin}"
MC_EXE="${MC_EXE:-$MC_BUILD/MinishCapRecomp.exe}"
FRAMES="${FRAMES:-800}"
DEMO="${DEMO:-walk}"
export PATH="/c/msys64/mingw64/bin:$PATH"

for f in "$ROM" "$BIOS" "$MC_EXE"; do
    [ -e "$f" ] || { echo "SKIP: missing $f"; exit 77; }  # 77 = ctest "skipped"
done
shopt -s nullglob
states=( "$MC_DIR"/roms/*.state* )
shopt -u nullglob
[ "${#states[@]}" -gt 0 ] || { echo "SKIP: no savestates in $MC_DIR/roms"; exit 77; }

demo_env=()
[ -n "$DEMO" ] && demo_env=(GBARECOMP_DEMO_INPUT="$DEMO")

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0; n=0
echo "== self-heal coverage sweep (demo=${DEMO:-none}, ${FRAMES}f/scene) =="
for st in "${states[@]}"; do
    name="$(basename "$st")"
    n=$((n + 1))
    taskkill //F //IM MinishCapRecomp.exe >/dev/null 2>&1 || true
    rm -rf "$MC_DIR/recomp_cache"
    cov="$work/$name.json"
    ( cd "$MC_DIR" && env GBARECOMP_SELFHEAL_RECOMPILE=1 GBARECOMP_HANG_WATCHDOG=0 \
        "${demo_env[@]}" GBARECOMP_COVERAGE_JSON="$cov" \
        "$MC_EXE" --bios "$BIOS" --rom "$ROM" --frames "$FRAMES" --no-window \
        --load-state "$st" >/dev/null 2>&1 ) || true
    if [ ! -s "$cov" ]; then
        echo "  $name: NO COVERAGE JSON (crash/hang?)"; fail=1; continue
    fi
    read -r verdict misses healed jt < <(python3 - "$cov" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(d.get("coverage", "?"), d.get("distinct_misses", -1),
      d.get("healed_native", -1), d.get("jump_table_candidate_regions", -1))
PY
)
    if [ "$verdict" = FULLY_STATIC ]; then
        echo "  $name: FULLY_STATIC"
    else
        echo "  $name: $verdict  misses=$misses healed=$healed jt_candidate_regions=$jt"
        python3 - "$cov" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for m in d.get("misses", []):
    flag = " [JT-candidate]" if m.get("jump_table_candidate") else ""
    print(f"      miss {m['pc']} {m['mode']} bridged={m['bridged']} "
          f"healed={m['healed']}{flag}")
PY
        fail=1
    fi
done

if [ "$fail" = 0 ]; then
    echo "PASS: all $n scene(s) FULLY_STATIC"
else
    echo "FAIL: a scene is NOT_STATIC — investigate the finder gap(s) above"
    echo "      (prefer a sized [[jump_table]] for JT-candidate runs)."
    exit 1
fi
