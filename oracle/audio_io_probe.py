#!/usr/bin/env python3
"""audio_io_probe.py — at frame N, dump every byte in the SOUND IO
region (0x60..0xB0) plus wave RAM (0x90..0xA0) from both sides and
report the divergent bytes.

Use this when the audio sample stream diverges and you want to know
whether the BIOS programmed the channels differently between native
and mGBA, or whether IO state matches and the bug is in mixer math.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "oracle"))
from diff_frame import JsonClient, spawn, BIOS_SMOKE, ORACLE, BIOS_PATH  # type: ignore


# (label, native-cmd, oracle-cmd, start-off, end-off-exclusive)
RANGES = [
    ("SOUND1 (0x60-0x68)",   "read_io", "read_emu_io", 0x60, 0x68),
    ("SOUND2 (0x68-0x70)",   "read_io", "read_emu_io", 0x68, 0x70),
    ("SOUND3 (0x70-0x78)",   "read_io", "read_emu_io", 0x70, 0x78),
    ("SOUND4 (0x78-0x80)",   "read_io", "read_emu_io", 0x78, 0x80),
    ("SOUNDCNT (0x80-0x88)", "read_io", "read_emu_io", 0x80, 0x88),
    ("SOUNDBIAS (0x88-0x90)","read_io", "read_emu_io", 0x88, 0x90),
    ("Wave RAM (0x90-0xA0)", "read_io", "read_emu_io", 0x90, 0xA0),
    ("FIFO_A (0xA0-0xA4)",   "read_io", "read_emu_io", 0xA0, 0xA4),
    ("FIFO_B (0xA4-0xA8)",   "read_io", "read_emu_io", 0xA4, 0xA8),
]


def dump_range(label, native, oracle, ncmd, ocmd, lo, hi) -> None:
    n_resp = native.call(cmd=ncmd, addr=lo, len=hi - lo)
    o_resp = oracle.call(cmd=ocmd, addr=lo, len=hi - lo)
    nb = bytes.fromhex(n_resp["data"])
    ob = bytes.fromhex(o_resp["data"])
    diffs = [(i, nb[i], ob[i]) for i in range(len(nb)) if nb[i] != ob[i]]
    n_hex = " ".join(f"{b:02x}" for b in nb)
    o_hex = " ".join(f"{b:02x}" for b in ob)
    status = "IDENTICAL" if not diffs else f"DIFF ({len(diffs)} bytes)"
    print(f"  {label}: {status}")
    print(f"    native: {n_hex}")
    print(f"    oracle: {o_hex}")
    if diffs:
        for off, n, o in diffs:
            print(f"    @0x{lo + off:03x}: native=0x{n:02x} oracle=0x{o:02x}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frame", type=int, default=60,
                    help="Step both sides to this frame before dumping.")
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    ap.add_argument("--no-spawn", action="store_true")
    ap.add_argument("--trace", action="store_true",
                    help="Also dump native audio_trace around sample 47674.")
    ap.add_argument("--target-sample", type=int, default=47674)
    args = ap.parse_args()

    procs: list[subprocess.Popen] = []
    try:
        if not args.no_spawn:
            procs.append(spawn(
                [str(BIOS_SMOKE), "--tcp", str(args.native_port)],
                "native"))
            procs.append(spawn(
                [str(ORACLE), "--bios", BIOS_PATH,
                 "--port", str(args.oracle_port)],
                "oracle"))
        native = JsonClient("127.0.0.1", args.native_port)
        oracle = JsonClient("127.0.0.1", args.oracle_port)
        try:
            # Step both to target frame.
            nf = native.call(cmd="frame")["frame"]
            of = oracle.call(cmd="frame")["frame"]
            while nf < args.frame:
                nf = native.call(cmd="step")["frame"]
            while of < args.frame:
                of = oracle.call(cmd="emu_step")["frame"]
            print(f"==> at native@{nf} oracle@{of}")
            for r in RANGES:
                dump_range(r[0], native, oracle, r[1], r[2], r[3], r[4])
            if args.trace:
                tr = native.call(cmd="audio_trace", max=1024)
                if not tr.get("ok"):
                    print(f"audio_trace failed: {tr}")
                else:
                    entries = tr.get("entries", [])
                    print(f"audio_trace: {len(entries)} entries")
                    target = args.target_sample
                    bases = sorted({e["base"] for e in entries})
                    print(f"  base range: min={bases[0]} max={bases[-1]} "
                          f"unique_bases={len(bases)}")
                    # Find entries whose sample_base is near target.
                    near = [e for e in entries
                            if abs(e["base"] - target) <= 200]
                    print(f"  entries within +/-200 of sample {target}:")
                    print("  base    fifo  until  start  slots  count  rem  sample")
                    for e in near[:30]:
                        print(f"  {e['base']:7d} {e['fifo']:5d} "
                              f"{e['until']:6d} {e['start']:6d} "
                              f"{e['slots']:6d} {e['count']:6d} "
                              f"{e['remaining']:4d} {e['sample']:6d}")
        finally:
            native.close()
            oracle.close()
    finally:
        for p in procs:
            try:
                p.terminate()
            except Exception:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
