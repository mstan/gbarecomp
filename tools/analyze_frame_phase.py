#!/usr/bin/env python3
"""Analyze a frame-phase CSV from the HP-002 probe (GBARECOMP_FRAME_PHASE).

Each row is one presented frame with the wall time spent in each phase of
the frame loop: guest execution, view-sync+render/copy, SDL present, audio
push, input pump, pacer wait, plus the game-thread overlay-compile delta.
The frame's total is ~= the present-to-present gap, so for every LATE frame
(total > threshold) the dominant column names the culprit phase directly.

Usage:
  python tools/analyze_frame_phase.py frame_phase.csv [--late-us 25000]
"""

import argparse
import csv
from collections import Counter

PHASES = ["guest_us", "render_us", "present_us", "audio_us", "pump_us",
          "pacer_us"]


def pct(sorted_vals, p):
    if not sorted_vals:
        return 0
    return sorted_vals[int(p * (len(sorted_vals) - 1))]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv_path")
    ap.add_argument("--late-us", type=int, default=25000,
                    help="frame-total threshold to classify late (default 25000)")
    args = ap.parse_args()

    rows = []
    with open(args.csv_path, newline="") as f:
        for r in csv.DictReader(f):
            row = {k: int(v) for k, v in r.items()}
            row["total"] = sum(row[p] for p in PHASES)
            rows.append(row)
    if not rows:
        print("empty CSV")
        return 1

    print(f"{args.csv_path}: {len(rows)} frames")
    print(f"\nper-phase distribution (us):")
    print(f"  {'phase':<12}{'p50':>8}{'p95':>8}{'p99':>8}{'max':>9}")
    for p in PHASES + ["compile_us", "total"]:
        vals = sorted(r[p] for r in rows)
        print(f"  {p:<12}{pct(vals,0.5):>8}{pct(vals,0.95):>8}"
              f"{pct(vals,0.99):>8}{vals[-1]:>9}")

    late = [r for r in rows if r["total"] > args.late_us]
    print(f"\nlate frames (total > {args.late_us} us): {len(late)} "
          f"({len(late)/len(rows):.1%})")
    if late:
        # For each late frame: which phase exceeds its own population p95 the
        # most, in absolute overage — that phase 'owns' the lateness.
        p95 = {p: pct(sorted(r[p] for r in rows), 0.95) for p in PHASES}
        owner = Counter()
        overage_sum = Counter()
        for r in late:
            over = {p: r[p] - p95[p] for p in PHASES}
            culprit = max(over, key=over.get)
            owner[culprit] += 1
            overage_sum[culprit] += max(0, over[culprit])
        print("  lateness ownership (phase, late frames owned, total overage ms):")
        for p, c in owner.most_common():
            print(f"    {p:<12}{c:>6}   {overage_sum[p]/1000.0:>8.1f}")
        # compile correlation
        cl = sum(1 for r in late if r["compile_us"] > 1000)
        print(f"  late frames with >1ms game-thread compile: {cl}/{len(late)}")
        # periodicity
        idx = [i for i, r in enumerate(rows) if r["total"] > args.late_us]
        sp = Counter(b - a for a, b in zip(idx, idx[1:]))
        print(f"  spacing histogram (frames between late): "
              f"{sorted(sp.items())[:15]}")
        print("\n  worst 8 frames:")
        print(f"  {'frame':>9}{'total':>8}" +
              "".join(f"{p.replace('_us',''):>9}" for p in PHASES) +
              f"{'compile':>9}")
        for r in sorted(late, key=lambda r: -r["total"])[:8]:
            print(f"  {r['frame']:>9}{r['total']:>8}" +
                  "".join(f"{r[p]:>9}" for p in PHASES) +
                  f"{r['compile_us']:>9}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
