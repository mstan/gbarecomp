#!/usr/bin/env python3
"""diff_mmio_nba.py -- Axis 4 (Memory / MMIO) recomp-vs-NanoBoyAdvance MMIO
write-sequence comparator. THE workhorse of the burndown.

Both sides run from COLD RESET and fill an always-on, non-destructive MMIO
write-trace ring ({cycle,addr,value,size,pc}). We pull both sequences and
compare element-by-element BY WRITE INDEX on the tuple (addr,value,size). That
ordering is offset-free: it does not depend on cycle counts agreeing (the cycle
clocks differ by construction), so it isolates a genuine MMIO-program divergence
from pure timing skew. cycle/pc are carried as secondary annotations only.

Reports, in burndown priority order:
  1. the FIRST write index where (addr,value,size) differ  (the root divergence)
  2. a per-address write-count tally diff  (who writes more/less)
  3. a per-address first-divergence        (first differing write per register)

Sync doctrine: both sides cold-boot the SAME ROM; we advance each by --frames
(recomp `step`, nba `run_frames`) then QUERY the rings. We never pause/step the
two into lockstep. The recomp must be freshly launched (cold) for index 0 to be
the reset write; this tool does NOT reset it (there is no cold-reset cmd) -- give
it a fresh process, or accept a `wrapped` note if the ring evicted the prefix.

Noisy registers (e.g. VCOUNT echoes, FIFO) can be filtered with --ignore.

Usage
-----
  # real sweep (MinishCap game build on :19842, INSN_TRACE not required):
  GBARECOMP_GAME_DIR=... <launch MinishCapRecomp.exe --tcp 19842 fresh>
  <launch nba_oracle.exe ... --port 19844>
  python oracle/diff_mmio_nba.py --frames 30

  # self-test (correctness of THIS comparator): NBA vs NBA == zero drift
  python oracle/diff_mmio_nba.py --self-nba --frames 20
"""
from __future__ import annotations

import argparse
import sys
from collections import Counter, defaultdict

import nba_common as nc


def parse_ignore(values):
    out = set()
    for v in values or []:
        for tok in v.replace(",", " ").split():
            out.add(int(tok, 0) & 0xFFFFFFFF)
    return out


def filt(records, ignore):
    if not ignore:
        return records
    return [r for r in records if r[0] not in ignore]


def fmt_rec(r):
    addr, val, size, cyc, pc = r
    return (f"addr=0x{addr:08x} val=0x{val:0{size*2}x} size={size} "
            f"(cyc={cyc} pc=0x{pc:08x})")


def compare(rec_a, rec_b, label_a, label_b, ignore, max_per_addr=8):
    a = filt(rec_a, ignore)
    b = filt(rec_b, ignore)
    print(f"\n=== MMIO WRITE-SEQUENCE DIFF: {label_a} vs {label_b} ===")
    print(f"  writes: {label_a}={len(a)}  {label_b}={len(b)}"
          + (f"  (ignoring {len(ignore)} addr)" if ignore else ""))

    # [1] first index divergence on (addr,value,size)
    n = min(len(a), len(b))
    first = None
    for i in range(n):
        if a[i][:3] != b[i][:3]:
            first = i
            break
    if first is None and len(a) != len(b):
        first = n

    if first is None:
        print(f"  [1] first divergence : NONE -- identical write stream over "
              f"{n} writes  OK")
        verdict = "ZERO-DRIFT"
    elif first == n:
        longer = label_a if len(a) > len(b) else label_b
        print(f"  [1] first divergence : at index {n} -- streams identical up to "
              f"the shorter length, but {longer} has "
              f"{abs(len(a)-len(b))} EXTRA trailing writes")
        for k in range(max(0, n - 3), n):
            print(f"        [{k}] {label_a}: {fmt_rec(a[k])}")
            print(f"             {label_b}: {fmt_rec(b[k])}")
        verdict = "LENGTH-MISMATCH"
    else:
        print(f"  [1] first divergence : at write index {first}")
        lo = max(0, first - 3)
        for k in range(lo, first):
            print(f"        [{k}] OK  {fmt_rec(a[k])}")
        print(f"     >> [{first}] {label_a}: {fmt_rec(a[first])}")
        print(f"        [{first}] {label_b}: {fmt_rec(b[first])}")
        da = []
        if a[first][0] != b[first][0]:
            da.append(f"addr 0x{a[first][0]:08x}!=0x{b[first][0]:08x}")
        if a[first][1] != b[first][1]:
            da.append(f"value 0x{a[first][1]:x}!=0x{b[first][1]:x}")
        if a[first][2] != b[first][2]:
            da.append(f"size {a[first][2]}!={b[first][2]}")
        print(f"        DIFF: {', '.join(da)}")
        verdict = "DRIFT"

    # [2] per-address write-count tally diff
    ca = Counter(r[0] for r in a)
    cb = Counter(r[0] for r in b)
    tally_diffs = sorted((set(ca) | set(cb)),
                         key=lambda ad: -abs(ca.get(ad, 0) - cb.get(ad, 0)))
    shown = [ad for ad in tally_diffs if ca.get(ad, 0) != cb.get(ad, 0)]
    print(f"  [2] write-count tally: {len(set(ca) | set(cb))} distinct addrs; "
          f"{len(shown)} with differing counts")
    for ad in shown[:20]:
        print(f"        0x{ad:08x}: {label_a}={ca.get(ad,0):<6} "
              f"{label_b}={cb.get(ad,0):<6} (d={ca.get(ad,0)-cb.get(ad,0):+d})")
    if len(shown) > 20:
        print(f"        ... {len(shown)-20} more")

    # [3] per-address first-divergence (per-register write subsequence)
    seq_a = defaultdict(list)
    seq_b = defaultdict(list)
    for r in a:
        seq_a[r[0]].append((r[1], r[2]))
    for r in b:
        seq_b[r[0]].append((r[1], r[2]))
    per_addr = []
    for ad in sorted(set(seq_a) | set(seq_b)):
        sa, sb = seq_a[ad], seq_b[ad]
        m = min(len(sa), len(sb))
        idx = None
        for j in range(m):
            if sa[j] != sb[j]:
                idx = j
                break
        if idx is None and len(sa) != len(sb):
            idx = m
        if idx is not None:
            per_addr.append((ad, idx, sa, sb))
    print(f"  [3] per-address first-divergence: {len(per_addr)} addrs diverge")
    for ad, idx, sa, sb in per_addr[:max_per_addr]:
        va = f"0x{sa[idx][0]:x}/s{sa[idx][1]}" if idx < len(sa) else "(end)"
        vb = f"0x{sb[idx][0]:x}/s{sb[idx][1]}" if idx < len(sb) else "(end)"
        print(f"        0x{ad:08x}: 1st differs at its write #{idx}  "
              f"{label_a}={va} {label_b}={vb}")
    if len(per_addr) > max_per_addr:
        print(f"        ... {len(per_addr)-max_per_addr} more")

    print(f"  VERDICT: {verdict}")
    return verdict


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recomp-port", type=int, default=19842)
    ap.add_argument("--nba-port", type=int, default=19844)
    ap.add_argument("--frames", type=int, default=30,
                    help="frames to advance each side from cold before pulling")
    ap.add_argument("--ignore", action="append",
                    help="addr(s) to filter, hex/dec, comma/space separated "
                         "(e.g. --ignore 0x4000006,0x40000a0)")
    ap.add_argument("--self-nba", action="store_true",
                    help="self-test: pull NBA twice and diff (must be zero-drift)")
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()
    ignore = parse_ignore(args.ignore)

    if args.self_nba:
        nba = nc.connect(args.host, args.nba_port, "nba")
        if not nba:
            return 2
        nba.call(cmd="reset")
        nba.call(cmd="run_frames", n=args.frames)
        a, wa = nc.pull_mmio_nba(nba)
        b, _ = nc.pull_mmio_nba(nba)  # same ring, second pull -> identical
        nba.close()
        if a is None:
            print("[err] nba mmio_cap returned no data")
            return 2
        if wa:
            print("[note] nba ring wrapped (cold-reset prefix evicted)")
        v = compare(a, b, "nba", "nba(self)", ignore)
        return 0 if v == "ZERO-DRIFT" else 1

    rec = nc.connect(args.host, args.recomp_port, "recomp")
    nba = nc.connect(args.host, args.nba_port, "nba")
    if not rec and not nba:
        return 2
    rec_recs = nba_recs = None
    if nba:
        nba.call(cmd="reset")
        nba.call(cmd="run_frames", n=args.frames)
        nba_recs, wn = nc.pull_mmio_nba(nba)
        if wn:
            print("[note] nba ring wrapped -- raise ring size or lower --frames")
    if rec:
        for _ in range(args.frames):
            try:
                rec.call(timeout=30, cmd="step")
            except Exception as e:
                print(f"[warn] recomp step failed/stalled: {e}")
                break
        rec_recs, wr = nc.pull_mmio_recomp(rec)
        if rec_recs is None:
            print("[skip] recomp mmio_cap unavailable on this build")
        elif wr:
            print("[note] recomp ring wrapped (not a fresh cold process?) -- "
                  "relaunch the runtime for an index-0-aligned compare")

    if rec:
        rec.close()
    if nba:
        nba.close()

    if rec_recs is not None and nba_recs is not None:
        v = compare(rec_recs, nba_recs, "recomp", "nba", ignore)
        return 0 if v == "ZERO-DRIFT" else 1
    if nba_recs is not None:
        print(f"\n[partial] nba-only: {len(nba_recs)} MMIO writes captured. "
              f"Provide a fresh recomp on :{args.recomp_port} for the diff.")
        return 0
    print("\n[partial] no comparable data (recomp side absent or empty).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
