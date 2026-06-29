#!/usr/bin/env python3
"""diff_cycle_nba.py -- Axis 2 (Cycle / timing) recomp-vs-NanoBoyAdvance cycle
ruler, offset-cancelled by consecutive-anchor DELTAS.

Pick a guest PC that recurs every frame (a frame-loop / IRQ-wait PC). On BOTH
sides we sample the cumulative guest-cycle stamp at each crossing of that PC for
H hits, then take consecutive-hit deltas d[i]=cyc[i+1]-cyc[i]. The delta
sequence cancels the boot offset (the absolute cyc[0] never enters the compare),
so a constant head-start does not register as drift -- only a difference in the
PER-INTERVAL cost does. That is exactly the WAITCNT / prefetch / DMA-cycle
divergence the burndown is hunting.

Reads:
  recomp (:19842) cyc_anchor{pc,hits} -> {armed,fp_count,count,cyc:[...]} -- a
    QUERY of the always-on insn-fingerprint ring. REQUIRES GBARECOMP_INSN_TRACE=1
    AND the per-game build (bios_smoke leaves the ring empty -> armed=0). The
    ring holds ~8 frames, so keep --frames within that window for a once-per-
    frame anchor so both sides sample the same boot-relative crossings.
  nba (:19844) cyc_anchor{pc,hits} -> {cyc:[...]} -- actively free-runs from the
    current (reset) state collecting the first H crossings.

Reports: per-hit recomp_delta vs nba_delta, drift onset (first interval beyond
--tol), steady delta (median each side), and drift rate (mean per-hit cycle skew
+ cumulative skew at the last hit).

Sync doctrine: anchor on a HARDWARE-recurring PC, never a frame number; never
pause/step the two into lockstep -- each free-runs its own ring, we query a
window and diff deltas.

Usage
-----
  # real sweep (needs INSN_TRACE + MinishCap game build on :19842):
  GBARECOMP_INSN_TRACE=1 <launch MinishCapRecomp.exe --tcp 19842>
  python oracle/diff_cycle_nba.py --pc 0x128 --hits 8 --frames 8

  # self-test (comparator correctness): NBA vs NBA == identical deltas
  python oracle/diff_cycle_nba.py --self-nba --pc 0x128 --hits 8
"""
from __future__ import annotations

import argparse
import statistics
import sys

import nba_common as nc


def deltas(cyc):
    return [cyc[i + 1] - cyc[i] for i in range(len(cyc) - 1)]


def nba_anchor(c, pc, hits, max_frames, reset=True):
    if reset:
        c.call(cmd="reset")
    r = c.call(cmd="cyc_anchor", pc=pc, hits=hits, max_cycles=280896 * max_frames)
    if not nc.ok(r):
        return None
    return [int(x) for x in r.get("cyc", [])]


def recomp_anchor(c, pc, hits, frames):
    # Drive the recomp forward so the always-on fp ring accumulates crossings,
    # THEN query the ring (we never arm-at-probe-time -- the ring is always on).
    for _ in range(frames):
        try:
            c.call(timeout=30, cmd="step")
        except Exception as e:
            print(f"[warn] recomp step failed/stalled: {e}")
            break
    r = c.call(cmd="cyc_anchor", pc=pc, hits=hits)
    if not nc.ok(r):
        return None, r
    return [int(x) for x in r.get("cyc", [])], r


def compare(rc, nbc, label_a, label_b, tol):
    print(f"\n=== CYCLE DELTA DIFF: {label_a} vs {label_b} ===")
    print(f"  anchor hits: {label_a}={len(rc)}  {label_b}={len(nbc)}")
    if len(rc) < 2 or len(nbc) < 2:
        print("  ! need >=2 hits per side to form deltas -- raise --hits/--frames "
              "or pick a more frequently-hit PC.")
        return "INSUFFICIENT"
    rd, nd = deltas(rc), deltas(nbc)
    n = min(len(rd), len(nd))
    print(f"  intervals compared: {n}")
    print(f"  {'hit':>4}  {label_a+'_d':>12}  {label_b+'_d':>12}  {'skew':>10}")
    onset = None
    cum = 0
    for i in range(n):
        sk = rd[i] - nd[i]
        cum += sk
        mark = ""
        if abs(sk) > tol and onset is None:
            onset = i
            mark = "  <-- drift onset"
        print(f"  {i:>4}  {rd[i]:>12}  {nd[i]:>12}  {sk:>+10}{mark}")
    steady_a = statistics.median(rd)
    steady_b = statistics.median(nd)
    mean_skew = (sum(rd[:n]) - sum(nd[:n])) / n
    print(f"  steady delta : {label_a}={steady_a}  {label_b}={steady_b}  "
          f"(median per interval)")
    print(f"  drift rate   : mean per-hit skew = {mean_skew:+.2f} cyc/interval; "
          f"cumulative skew at hit {n} = {cum:+d} cyc")
    if onset is None and steady_a == steady_b:
        print("  VERDICT: ZERO-DRIFT (delta sequences identical)")
        return "ZERO-DRIFT"
    if onset is None:
        print(f"  VERDICT: BOUNDED (no interval beyond +-{tol}, but medians "
              f"differ by {steady_a-steady_b:+d})")
        return "BOUNDED"
    print(f"  VERDICT: DRIFT (onset at interval {onset})")
    return "DRIFT"


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recomp-port", type=int, default=19842)
    ap.add_argument("--nba-port", type=int, default=19844)
    ap.add_argument("--pc", type=lambda s: int(s, 0), default=0x128,
                    help="guest PC anchor (recurs each frame). default 0x128 "
                         "(BIOS frame-loop)")
    ap.add_argument("--hits", type=int, default=8,
                    help="anchor crossings to sample per side")
    ap.add_argument("--frames", type=int, default=8,
                    help="frames to advance the recomp before querying its ring")
    ap.add_argument("--tol", type=int, default=0,
                    help="per-interval cycle tolerance before flagging drift")
    ap.add_argument("--self-nba", action="store_true",
                    help="self-test: NBA vs NBA (identical delta sequences)")
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()
    maxf = max(args.hits + 4, args.frames + 4) * 2

    if args.self_nba:
        nba = nc.connect(args.host, args.nba_port, "nba")
        if not nba:
            return 2
        a = nba_anchor(nba, args.pc, args.hits, maxf)
        b = nba_anchor(nba, args.pc, args.hits, maxf)
        nba.close()
        if not a or not b:
            print(f"[err] nba cyc_anchor returned no hits at pc=0x{args.pc:08x} "
                  f"-- pick a PC that recurs (try 0x128).")
            return 2
        v = compare(a, b, "nba", "nba(self)", args.tol)
        return 0 if v == "ZERO-DRIFT" else 1

    rec = nc.connect(args.host, args.recomp_port, "recomp")
    nba = nc.connect(args.host, args.nba_port, "nba")
    if not nba:
        print("[abort] nba oracle is the reference ruler; cannot proceed without it.")
        return 2
    nbc = nba_anchor(nba, args.pc, args.hits, maxf)
    rc = None
    meta = None
    if rec:
        rc, meta = recomp_anchor(rec, args.pc, args.hits, args.frames)
        rec.close()
    nba.close()

    if rc is None or len(rc) < 2:
        if meta is None:
            print("\n[partial] recomp cyc_anchor unavailable on this build.")
        elif not meta.get("armed"):
            print(f"\n[pending] recomp cyc_anchor armed=0 (fp_count="
                  f"{meta.get('fp_count')}). The insn-fingerprint ring is off: "
                  f"relaunch with GBARECOMP_INSN_TRACE=1 AND the per-game build.")
        else:
            print(f"\n[pending] recomp ring armed (fp_count={meta.get('fp_count')}) "
                  f"but pc=0x{args.pc:08x} was crossed {len(rc or [])} time(s) in "
                  f"the resident window. Pick a PC the build actually loops on, or "
                  f"raise --frames (ring holds ~8 frames). nba-only below.")
        if nbc:
            nd = deltas(nbc)
            print(f"[nba] pc=0x{args.pc:08x} hits={len(nbc)} "
                  f"steady_delta={statistics.median(nd) if nd else 'n/a'}")
        return 0

    v = compare(rc, nbc, "recomp", "nba", args.tol)
    return 0 if v == "ZERO-DRIFT" else 1


if __name__ == "__main__":
    sys.exit(main())
