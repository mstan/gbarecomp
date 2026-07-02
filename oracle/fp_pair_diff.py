#!/usr/bin/env python3
"""fp_pair_diff.py — diff two pre-dumped fingerprint rings (GBARECOMP_FP_SAVE) to
name the first instruction whose per-instruction CYCLE charge differs between two
backends. Purpose-built for the gba-cosim pairing: the SAME binary run twice
(recomp vs GBARECOMP_FORCE_INTERP=1), each with GBARECOMP_INSN_TRACE=1 +
GBARECOMP_FP_SAVE, so both fingerprint the identical instruction stream from boot.

Each record is (cumulative-cycles-BEFORE-this-instruction, pc, cpsr, r[0..15]). Both
streams execute the same instructions in the same order until the real divergence,
so walking by index: the FIRST index where `cycles` differs means the instruction
at index-1 was charged a different number of cycles by the two backends — that is
the mis-charged instruction. (If `pc` also differs at that index, it is a genuine
control-flow split, not a cycle-charge one.)

  python oracle/fp_pair_diff.py recomp.fp interp.fp [--ctx 12]
"""
import argparse, struct, sys

HDR = struct.Struct("<IIQ")          # magic, entry_size, count
REC = struct.Struct("<QII16I")       # u64 cycles, u32 pc, u32 cpsr, 16*u32 regs
MAGIC = 0x31504647                   # 'GFP1'


def load_fp(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, esz, count = HDR.unpack_from(data, 0)
    if magic != MAGIC or esz != REC.size:
        sys.exit(f"{path}: bad fp file (magic={magic:#x} esz={esz}, want {MAGIC:#x}/{REC.size})")
    recs = []
    off = HDR.size
    for _ in range(count):
        cyc, pc, cpsr = REC.unpack_from(data, off)[:3]
        recs.append((cyc, pc, cpsr))
        off += REC.size
    return recs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b")
    ap.add_argument("--label-a", default="recomp"); ap.add_argument("--label-b", default="interp")
    ap.add_argument("--ctx", type=int, default=12)
    args = ap.parse_args()

    a = load_fp(args.a); b = load_fp(args.b)
    print(f"{args.label_a}: {len(a)} records ({a[0][0]}..{a[-1][0]} cyc)  "
          f"{args.label_b}: {len(b)} records ({b[0][0]}..{b[-1][0]} cyc)", flush=True)

    n = min(len(a), len(b))
    first = None
    for i in range(n):
        if a[i][0] != b[i][0] or a[i][1] != b[i][1]:
            first = i
            break
    if first is None:
        print(f"no (cycle,pc) divergence over {n} aligned records — "
              f"streams identical in the overlap.", flush=True)
        return

    kind = "CONTROL-FLOW (pc differs)" if a[first][1] != b[first][1] else "CYCLE-CHARGE (pc same, cumulative cycles differ)"
    print(f"\n*** FIRST DIVERGENCE at index {first} — {kind} ***", flush=True)
    ca, pa, _ = a[first]; cb, pb, _ = b[first]
    print(f"  idx {first}: {args.label_a} cyc={ca} pc={pa:#010x}   "
          f"{args.label_b} cyc={cb} pc={pb:#010x}   dcyc={cb-ca:+d}", flush=True)
    if a[first][1] == b[first][1] and first > 0:
        cpc = a[first-1][1]
        da = a[first][0] - a[first-1][0]
        db = b[first][0] - b[first-1][0]
        print(f"  => MIS-CHARGED INSTRUCTION at idx {first-1}, pc={cpc:#010x}: "
              f"{args.label_a} charged {da} cyc, {args.label_b} charged {db} cyc "
              f"(delta {db-da:+d})", flush=True)
    lo = max(0, first - args.ctx)
    print(f"  --- context idx {lo}..{first+2} (idx: pc  A_cyc  B_cyc  A_dcyc  B_dcyc) ---", flush=True)
    for i in range(lo, min(n, first + 3)):
        da = a[i][0] - a[i-1][0] if i > 0 else 0
        db = b[i][0] - b[i-1][0] if i > 0 else 0
        mark = "  <-- FIRST DIFF" if i == first else ("  <-- culprit" if i == first-1 else "")
        flag = "" if (a[i][0]==b[i][0] and a[i][1]==b[i][1]) else " *"
        print(f"  {i:6d}: pc={a[i][1]:#010x}  A={a[i][0]}  B={b[i][0]}  "
              f"Adc={da} Bdc={db}{flag}{mark}", flush=True)


if __name__ == "__main__":
    main()
