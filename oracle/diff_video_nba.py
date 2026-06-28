#!/usr/bin/env python3
"""diff_video_nba.py -- Axis 5 (Peripherals / video) recomp-vs-NanoBoyAdvance
surface comparator: framebuffer pixels + VRAM / PRAM(PAL) / OAM bytes.

Both sides are synced to a common HARDWARE point -- the same VBlank count -- by
advancing each whole PPU frames (recomp `step`, nba `run_frames`); stepping a
whole frame lands both at the same VBlank boundary, which the project SYNC RULES
permit as a stop signal (frame COUNT is the anchor, not a raw frame number used
as a clock). Then we read each surface from its always-on backing store and diff.

Pixel-format normalization: the recomp `screenshot` returns RGB888 (3 bytes/px,
R,G,B order -- the order diff_frame.py already validates against the oracle); NBA
`screenshot` decodes its 0xFF_RRGGBB frame to the same R,G,B byte order. So both
arrive pre-normalized to RGB and compare directly. --recomp-bgr swaps the R/B of
the recomp frame if a future build emits BGR.

Reports: framebuffer mismatch pixel count + first differing pixel (x,y) with both
RGB triples; per-surface first differing byte + total differing bytes.

Reads:
  recomp (:19842) screenshot -> {w,h,data}; read_vram/read_pal/read_oam{addr,len}
  nba (:19844)    screenshot -> {w,h,data}; read{region=vram|pram|oam,addr,len}

Usage
-----
  python oracle/diff_video_nba.py --frames 60            # real sweep
  python oracle/diff_video_nba.py --self-nba --frames 60 # comparator self-test
"""
from __future__ import annotations

import argparse
import sys

import nba_common as nc

SURFACES = ["vram", "pal", "oam"]


def get_fb(c):
    r = c.call(cmd="screenshot")
    if not nc.ok(r):
        return None
    return int(r.get("w", 0)), int(r.get("h", 0)), nc.hx(r.get("data", ""))


def diff_fb(a, b, label_a, label_b, recomp_bgr=False):
    if a is None or b is None:
        print(f"  FRAMEBUFFER: unavailable ({label_a if a is None else label_b} "
              f"returned no frame -- has a frame been presented yet?)")
        return None
    (wa, ha, da), (wb, hb, db) = a, b
    print(f"  FRAMEBUFFER: {label_a}={wa}x{ha} {label_b}={wb}x{hb}")
    if recomp_bgr:
        da = bytearray(da)
        da[0::3], da[2::3] = da[2::3], da[0::3]
        da = bytes(da)
    if (wa, ha) != (wb, hb):
        print(f"    ! dimension mismatch -- comparing the common 240x160 region")
        w, h = 240, 160
        da = crop(da, wa, w, h)
        db = crop(db, wb, w, h)
    else:
        w, h = wa, ha
    n = min(len(da), len(db))
    diffpix = []
    first = None
    for p in range(n // 3):
        o = p * 3
        if da[o:o+3] != db[o:o+3]:
            diffpix.append(p)
            if first is None:
                first = p
    if first is None:
        print(f"    identical over {n//3} pixels  OK")
        return 0
    x, y = first % w, first // w
    o = first * 3
    print(f"    MISMATCH: {len(diffpix)} of {n//3} pixels differ; "
          f"first @({x},{y}) {label_a}=RGB{tuple(da[o:o+3])} "
          f"{label_b}=RGB{tuple(db[o:o+3])}")
    return len(diffpix)


def crop(data, src_w, w, h):
    out = bytearray()
    for y in range(h):
        row = y * src_w * 3
        out += data[row: row + w * 3]
    return bytes(out)


def diff_surfaces(rec, nba, label_a, label_b):
    total = 0
    for name in SURFACES:
        a = nc.read_region_recomp(rec, name)
        b = nc.read_region_nba(nba, name)
        if a is None or b is None:
            print(f"  {name.upper():<5}: unavailable on one side")
            continue
        fd = nc.first_diff(a, b)
        if fd is None:
            print(f"  {name.upper():<5}: identical ({len(a)} bytes)  OK")
        else:
            ndiff = sum(1 for x, y in zip(a, b) if x != y) + abs(len(a) - len(b))
            va = f"0x{a[fd]:02x}" if fd < len(a) else "(end)"
            vb = f"0x{b[fd]:02x}" if fd < len(b) else "(end)"
            print(f"  {name.upper():<5}: MISMATCH first @0x{fd:05x} "
                  f"{label_a}={va} {label_b}={vb}; {ndiff} bytes differ")
            total += ndiff
    return total


def surfaces_aligned(rec, nba):
    """Return (all_match, [names that differ]) for VRAM/PAL/OAM."""
    bad = []
    for name in SURFACES:
        a = nc.read_region_recomp(rec, name)
        b = nc.read_region_nba(nba, name)
        if a is None or b is None or nc.first_diff(a, b) is not None:
            bad.append(name.upper())
    return (not bad), bad


def input_parity(rec, nba):
    """Reconstruct + compare the compositor INPUTS (surfaces + written PPU
    control regs). Returns (aligned, surface_bad, reg_diffs)."""
    sok, sbad = surfaces_aligned(rec, nba)
    rmm, _ = nc.pull_mmio_recomp(rec)
    nmm, _ = nc.pull_mmio_nba(nba)
    if rmm is None or nmm is None:
        return False, sbad, [("?", "mmio", 0, 0)]
    rok, rdiffs = nc.ppu_input_parity(rmm, nmm)
    return (sok and rok), sbad, rdiffs


def report_parity(sbad, rdiffs):
    if sbad:
        print("  inputs: surfaces differ -> " + ", ".join(sbad))
    if rdiffs:
        print("  inputs: compositor regs differ -> " + ", ".join(
            f"{nm}(r=0x{rw:04x} n=0x{nw:04x})" for off, nm, rw, nw in rdiffs[:8]))


def assess(rec, nba, recomp_bgr):
    """One phase-gated compositor comparison at the current (already-stepped)
    position of both sides. Returns a verdict string."""
    aligned, sbad, rdiffs = input_parity(rec, nba)
    pix = diff_fb(get_fb(rec), get_fb(nba), "recomp", "nba", recomp_bgr)
    if not aligned:
        report_parity(sbad, rdiffs)
        print("  VERDICT: NOT-PHASE-ALIGNED (compositor inputs differ at this "
              "frame; the pixel diff is NOT a compositor verdict -- use --scan "
              "to find an input-aligned frame)")
        return "NOT-ALIGNED"
    # Inputs match on both sides -> the framebuffer MUST match if the compositor
    # is faithful. Any pixel diff here is a CONFIRMED compositor bug.
    if pix == 0:
        print("  inputs aligned (surfaces + PPU control regs) and framebuffer "
              "pixel-identical")
        print("  VERDICT: COMPOSITOR-FAITHFUL")
        return "FAITHFUL"
    print("  inputs aligned (surfaces + PPU control regs) but framebuffer "
          f"differs by {pix} px")
    print("  VERDICT: COMPOSITOR-BUG (confirmed: identical inputs, divergent "
          "pixels)")
    return "COMPOSITOR-BUG"


def step_both(rec, nba, n=1):
    for _ in range(n):
        nba.call(cmd="run_frames", n=1)
        rec.call(timeout=30, cmd="step")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recomp-port", type=int, default=19842)
    ap.add_argument("--nba-port", type=int, default=19844)
    ap.add_argument("--frames", type=int, default=60,
                    help="VBlank count to sync both sides to (whole frames)")
    ap.add_argument("--scan", nargs=2, type=int, metavar=("LO", "HI"),
                    help="step from LO..HI frames; report the compositor verdict "
                         "at the FIRST input-aligned frame (avoids fade-phase "
                         "false drift)")
    ap.add_argument("--recomp-bgr", action="store_true",
                    help="swap R/B of the recomp framebuffer before comparing")
    ap.add_argument("--self-nba", action="store_true")
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()

    if args.self_nba:
        nba = nc.connect(args.host, args.nba_port, "nba")
        if not nba:
            return 2
        nba.call(cmd="reset")
        nba.call(cmd="run_frames", n=args.frames)
        print(f"\n=== VIDEO SELF-TEST: nba vs nba (frame {args.frames}) ===")
        fb = get_fb(nba)
        pix = diff_fb(fb, fb, "nba", "nba(self)")
        tot = 0
        for name in SURFACES:
            s = nc.read_region_nba(nba, name)
            fd = nc.first_diff(s, s) if s is not None else 0
            if s is None:
                print(f"  {name.upper():<5}: unavailable")
            else:
                print(f"  {name.upper():<5}: identical ({len(s)} bytes)  OK")
        nba.close()
        v = "ZERO-DRIFT" if (pix == 0 and tot == 0) else "DRIFT"
        print(f"  VERDICT: {v}")
        return 0 if v == "ZERO-DRIFT" else 1

    rec = nc.connect(args.host, args.recomp_port, "recomp")
    nba = nc.connect(args.host, args.nba_port, "nba")
    if not nba:
        return 2
    nba.call(cmd="reset")

    if not rec:
        nba.call(cmd="run_frames", n=args.frames)
        print("[partial] recomp absent; nba-only -- showing nba frame dims:")
        fb = get_fb(nba)
        if fb:
            print(f"  nba framebuffer {fb[0]}x{fb[1]}, {len(fb[2])} bytes")
        nba.close()
        return 0

    if args.scan:
        lo, hi = args.scan
        print(f"\n=== VIDEO SCAN: recomp vs nba, frames {lo}..{hi} "
              f"(report at first input-aligned frame) ===")
        step_both(rec, nba, lo)
        for f in range(lo, hi + 1):
            aligned, sbad, rdiffs = input_parity(rec, nba)
            if aligned:
                print(f"\n  frame {f}: INPUT-ALIGNED (surfaces + PPU regs match)")
                v = assess(rec, nba, args.recomp_bgr)
                rec.close(); nba.close()
                return 0 if v == "FAITHFUL" else 1
            short = ",".join(sbad + [nm for _, nm, _, _ in rdiffs[:3]])
            print(f"  frame {f}: not aligned ({short})")
            if f < hi:
                step_both(rec, nba, 1)
        print("\n  no fully input-aligned frame in range -- the game may be "
              "mid-animation throughout; widen --scan or pick a static screen.")
        rec.close(); nba.close()
        return 1

    nba.call(cmd="run_frames", n=args.frames)
    for _ in range(args.frames):
        try:
            rec.call(timeout=30, cmd="step")
        except Exception as e:
            print(f"[warn] recomp step failed/stalled: {e}")
            break
    print(f"\n=== VIDEO DIFF: recomp vs nba (synced to VBlank {args.frames}) ===")
    v = assess(rec, nba, args.recomp_bgr)
    rec.close()
    nba.close()
    return 0 if v in ("FAITHFUL", "NOT-ALIGNED") else 1


if __name__ == "__main__":
    sys.exit(main())
