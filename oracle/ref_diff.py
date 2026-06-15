#!/usr/bin/env python3
"""ref_diff.py — differential-oracle diff for the gbaref/snesref/mdref method.

Compares two per-frame WRAM-change traces (JSONL, records
{"f":N,"adr":"0x...","old":"0x..","val":"0x.."}):
  - the RECOMP run   (gbarecomp runtime, GBARECOMP_WRAM_TRACE=...)
  - the REFERENCE run (gbaref + a libretro GBA core, e.g. mGBA)

Both are captured by a human playing the SAME scene on each side — there is NO
frame alignment and NO scripted input (that always fails). So this does not diff
by frame number. Instead it answers value/order questions per the recomp-template
"order + state + caller" rule:

  --addr 0xADDR   : show the SEQUENCE of values each side writes to one address
                    (e.g. an object field). "reference settles to 0x1103, recomp
                    settles to 0x0000" is an alignment-free divergence signal.
  --final         : for every traced address, the LAST value each side wrote;
                    list the addresses whose final value differs (the corrupt
                    state at the moment of capture). This is the fast first pass:
                    park both at the same scene, dump, diff finals.
  --watch a-b     : restrict to a GBA address range.

  python oracle/ref_diff.py recomp.jsonl reference.jsonl --final --watch 0x03001700-0x030017ff
  python oracle/ref_diff.py recomp.jsonl reference.jsonl --addr 0x0300174a
"""
from __future__ import annotations
import argparse, json, sys


def load(path):
    """Return (final{addr:val}, seq{addr:[vals]}) from a JSONL trace."""
    final, seq = {}, {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
                a = int(r["adr"], 16); v = int(r["val"], 16)
            except Exception:
                continue
            final[a] = v
            seq.setdefault(a, []).append(v)
    return final, seq


def in_range(a, rng):
    return rng is None or (rng[0] <= a <= rng[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recomp")
    ap.add_argument("reference")
    ap.add_argument("--addr", type=lambda s: int(s, 16), default=None,
                    help="show the value sequence each side wrote to this GBA addr")
    ap.add_argument("--final", action="store_true",
                    help="list addresses whose LAST-written value differs")
    ap.add_argument("--watch", default=None,
                    help="restrict to a-b, e.g. 0x03001700-0x030017ff")
    args = ap.parse_args()
    rng = None
    if args.watch:
        lo, hi = args.watch.split("-")
        rng = (int(lo, 16), int(hi, 16))

    rfin, rseq = load(args.recomp)
    ofin, oseq = load(args.reference)
    print(f"recomp:    {len(rfin)} addrs, {sum(len(v) for v in rseq.values())} writes  ({args.recomp})")
    print(f"reference: {len(ofin)} addrs, {sum(len(v) for v in oseq.values())} writes  ({args.reference})")

    if args.addr is not None:
        a = args.addr
        print(f"\n== value sequence @0x{a:08x} ==")
        print(f"  recomp:    {[f'0x{v:02x}' for v in rseq.get(a, [])][:60]}")
        print(f"  reference: {[f'0x{v:02x}' for v in oseq.get(a, [])][:60]}")
        rf, of = rfin.get(a), ofin.get(a)
        print(f"  final:     recomp={('0x%02x'%rf) if rf is not None else '—'}  "
              f"reference={('0x%02x'%of) if of is not None else '—'}  "
              f"{'<<< DIFFER' if rf != of else 'match'}")
        return 0

    if args.final:
        addrs = sorted(set(rfin) | set(ofin))
        diffs = [a for a in addrs if in_range(a, rng) and rfin.get(a) != ofin.get(a)]
        print(f"\n== final-value divergences ({len(diffs)} addrs"
              f"{' in '+args.watch if rng else ''}) ==")
        for a in diffs:
            rf, of = rfin.get(a), ofin.get(a)
            print(f"  0x{a:08x}: recomp={('0x%02x'%rf) if rf is not None else '--':>4}  "
                  f"reference={('0x%02x'%of) if of is not None else '--':>4}")
        if not diffs:
            print("  (none — states agree in range; widen --watch or pick another scene)")
        return 0

    print("\nspecify --final or --addr 0xADDR")
    return 1


if __name__ == "__main__":
    sys.exit(main())
