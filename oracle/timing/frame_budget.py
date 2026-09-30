"""Per-frame CPU budget from SWI logs: how far past VBlank a game's main loop
runs before it waits for the next one (VBlankIntrWait, SWI 5).

Inputs are SWI logs stamped in cycles since reset at the start of the SWI:
bios_smoke's GBARECOMP_SWI_LOG (<prefix>.interp.csv: seq,cycles,imm,...) and
oracle/timing's `gba_timing_oracle <rom> --swi-log <bios> <frames>`
(seq,cycles,imm). Both boot through the real BIOS from reset, so frame
phase is cycles mod 280896 with line 0 at cycle 0; VBlank begins at line 160.

    python frame_budget.py [--swi N] label=path [label=path ...]

--swi picks the wait SWI (default 5, VBlankIntrWait; 2 for games that Halt
in their own VBlank-flag loop). The first such call after each VBlank ends
that frame's busy period.

For every log it prints the number of VBlankIntrWait calls, the busy cycles
from the VBlank that ended the previous wait to each call (median / mean /
p90), and for every log after the first the per-frame difference to the
first (the reference), aligned by frame index.
"""
import csv
import statistics
import sys

FRAME = 280896
VBLANK = 160 * 1232


WAIT_SWI = 5


def load(path):
    waits = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            imm = int(row["imm"], 0)
            if imm not in (WAIT_SWI, WAIT_SWI << 16):
                continue
            cyc = int(row["cycles"])
            frame, phase = divmod(cyc, FRAME)
            busy = (phase - VBLANK) % FRAME
            # Frame whose VBlank started the busy period.
            start = frame if phase >= VBLANK else frame - 1
            waits.setdefault(start, busy)
    return waits


def summary(busy):
    values = sorted(busy)
    p90 = values[int(0.9 * (len(values) - 1))]
    return (f"median {statistics.median(values):.0f}  mean {statistics.fmean(values):.0f}  "
            f"p90 {p90}  ({statistics.fmean(values) / FRAME:.1%} of a frame)")


args = sys.argv[1:]
if args[:1] == ["--swi"]:
    WAIT_SWI = int(args[1], 0)
    args = args[2:]
logs = []
for arg in args:
    label, path = arg.split("=", 1)
    logs.append((label, load(path)))
ref_label, ref = logs[0]
for label, waits in logs:
    frames = sorted(waits)
    print(f"{label}: {len(waits)} SWI {WAIT_SWI:#x} frames {frames[0]}..{frames[-1]}; "
          f"busy after VBlank: {summary(list(waits.values()))}")
for label, waits in logs[1:]:
    common = sorted(set(waits) & set(ref))
    diffs = [waits[k] - ref[k] for k in common]
    exact = sum(1 for d in diffs if d == 0)
    print(f"{label} - {ref_label}: {len(common)} common frames, {exact} identical, "
          f"busy diff median {statistics.median(diffs):.0f} mean {statistics.fmean(diffs):.0f} "
          f"min {min(diffs)} max {max(diffs)}; "
          f"ratio of means {statistics.fmean(waits[k] for k in common) / statistics.fmean(ref[k] for k in common):.3f}")
