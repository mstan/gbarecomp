#!/usr/bin/env python3
"""Analyze a present-cadence CSV from the MC-WS-002 probe.

The runtime's HostWindow keeps an always-on ring of every SDL_RenderPresent
(wall time blocked inside the call, entry-to-entry gap, DWM composition
refresh counter). Running the game with GBARECOMP_PRESENT_CADENCE=1 dumps the
ring to a CSV at window close (default ./_present_cadence.csv, override with
GBARECOMP_PRESENT_CADENCE_DUMP). This script reads that CSV and answers the
questions that discriminate the MC-WS-002 tearing/judder mechanisms:

  1. Is vsync actually blocking?   (block_us ~ a refresh period vs ~0)
  2. What is the refresh cadence?  (rdelta histogram: 1,1,1 = synced per
     refresh; 2/3 alternation on a 164 Hz panel = 59.73->164 pulldown
     judder; 0 rows = a present replaced inside one refresh)
  3. Is VRR engaged?               (rdelta ~1 per present at ~16.74 ms gaps
     on a high-Hz panel = the compositor is refreshing in step with us)
  4. How uneven is delivery?       (gap jitter, long-gap outliers)

Windowed vs fullscreen segments are analyzed separately (the `fullscreen`
column), since independent-flip/VRR behavior differs between them.

Usage:
  python tools/analyze_present_cadence.py _present_cadence.csv [--period-ms 16.742]
"""

import argparse
import csv
import statistics
import sys

GBA_PERIOD_MS = 280896.0 / 16777216.0 * 1000.0  # 16.7427 ms (59.7275 Hz)


def pct(sorted_vals, p):
    if not sorted_vals:
        return 0.0
    k = int(p * (len(sorted_vals) - 1))
    return sorted_vals[k]


def analyze_segment(rows, label, period_ms):
    n = len(rows)
    print(f"\n=== segment: {label} ({n} presents) ===")
    if n < 10:
        print("  too few presents to analyze")
        return

    gaps = [r["gap_us"] for r in rows if r["gap_us"] > 0]
    blocks = [r["block_us"] for r in rows]
    rdeltas = [r["rdelta"] for r in rows if r["rdelta"] >= 0]

    gaps_s = sorted(gaps)
    blocks_s = sorted(blocks)

    dur_s = sum(gaps) / 1e6
    fps = len(gaps) / dur_s if dur_s > 0 else 0.0
    print(f"  duration={dur_s:.1f}s  fps={fps:.3f}  (target {1000.0/period_ms:.4f})")

    # 1) vsync blocking?
    b50 = pct(blocks_s, 0.50)
    b95 = pct(blocks_s, 0.95)
    frac_tiny = sum(1 for b in blocks if b < 1000) / n
    print(f"  block_us: p50={b50} p95={b95} max={max(blocks_s)} "
          f"frac<1ms={frac_tiny:.2%}")
    if frac_tiny > 0.90:
        print("  -> VERDICT: vsync is NOT blocking (presents return immediately)."
              "\n     Pacing is software-only; tear-prone under independent flip.")
    elif b50 > 2000:
        print("  -> VERDICT: vsync IS blocking (present waits for a refresh"
              " boundary or VRR flip).")
    else:
        print("  -> VERDICT: mixed blocking — check the histogram below.")

    # 2/3) refresh cadence
    if rdeltas:
        hist = {}
        for d in rdeltas:
            hist[d] = hist.get(d, 0) + 1
        hist_str = " ".join(f"{k}:{v}" for k, v in sorted(hist.items()))
        print(f"  rdelta histogram: {{{hist_str}}}  (n={len(rdeltas)})")
        total = len(rdeltas)
        ones = hist.get(1, 0) / total
        zeros = hist.get(0, 0) / total
        twothree = (hist.get(2, 0) + hist.get(3, 0)) / total
        g50_ms = pct(gaps_s, 0.50) / 1000.0
        if ones > 0.90 and abs(g50_ms - period_ms) < 1.5:
            if frac_tiny > 0.90:
                print("  -> VERDICT: rdelta=1 per present BUT presents do not"
                      " block — the DWM composition clock is likely IDLING at"
                      " the present rate (windowed/occluded/headless), not"
                      " VRR. Treat cadence as UNSYNCED; rely on block_us.")
            else:
                print("  -> VERDICT: 1 refresh per present at the GBA period"
                      " with blocking presents — VRR IS ENGAGED (panel"
                      " refreshing in step with emulation). Cadence judder"
                      " should be gone; any residual artifact is elsewhere.")
        elif ones > 0.90:
            print(f"  -> VERDICT: synced 1:1 with composition at ~{1000.0/g50_ms:.1f}"
                  " Hz — fixed-rate sync (typical 60 Hz panel). Expect one"
                  " duplicated frame beat every ~1/(rate-59.7275) s.")
        elif twothree > 0.80:
            print("  -> VERDICT: 2/3-refresh pulldown — classic 59.73-on-high-Hz"
                  " CADENCE JUDDER. Frames dwell unevenly (2 vs 3 refreshes);"
                  " this is the waviness/shear mechanism.")
        if zeros > 0.02:
            print(f"  -> NOTE: {zeros:.1%} presents landed inside the same DWM"
                  " refresh as the previous one (frame replaced before scanout).")
    else:
        print("  rdelta: unavailable (no DWM data — non-Windows or query failed)")

    # 4) delivery evenness
    g50 = pct(gaps_s, 0.50)
    g95 = pct(gaps_s, 0.95)
    sd = statistics.pstdev(gaps) if len(gaps) > 1 else 0.0
    long_gaps = sum(1 for g in gaps if g > 1.5 * g50)
    sub_period = sum(1 for g in gaps if g < 0.7 * period_ms * 1000)
    print(f"  gap_us: p50={g50} p95={g95} sd={sd:.0f} "
          f">1.5xp50={long_gaps} ({long_gaps/len(gaps):.2%}) "
          f"sub-period={sub_period}")
    if sub_period > 0:
        print(f"  -> NOTE: {sub_period} gaps shorter than the pacer period —"
              " these can only come from vsync releasing on a refresh boundary"
              " (quantization), not from the FramePacer.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv_path", help="CSV written by GBARECOMP_PRESENT_CADENCE=1")
    ap.add_argument("--period-ms", type=float, default=GBA_PERIOD_MS,
                    help="expected content frame period (default GBA %(default).4f)")
    args = ap.parse_args()

    rows = []
    with open(args.csv_path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append({
                "idx": int(r["idx"]),
                "t_ms": float(r["t_ms"]),
                "gap_us": int(r["gap_us"]),
                "block_us": int(r["block_us"]),
                "dwm_refresh": int(r["dwm_refresh"]),
                "rdelta": int(r["rdelta"]),
                "fullscreen": int(r["fullscreen"]),
            })
    if not rows:
        print("empty CSV", file=sys.stderr)
        return 1

    print(f"{args.csv_path}: {len(rows)} presents, "
          f"{rows[-1]['t_ms']/1000.0:.1f}s span")

    analyze_segment(rows, "ALL", args.period_ms)
    win_rows = [r for r in rows if not r["fullscreen"]]
    fs_rows = [r for r in rows if r["fullscreen"]]
    if win_rows and fs_rows:
        analyze_segment(win_rows, "windowed", args.period_ms)
        analyze_segment(fs_rows, "fullscreen", args.period_ms)
    return 0


if __name__ == "__main__":
    sys.exit(main())
