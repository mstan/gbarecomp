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
    ("DMA0-3 (0xB0-0xE0)",   "read_io", "read_emu_io", 0xB0, 0xE0),
    ("Timers (0x100-0x110)", "read_io", "read_emu_io", 0x100, 0x110),
]

MEM_RANGES = [
    ("IRQ audio reload state @03001188", 0x03001188, 32),
    ("DMA1 PCM ring @03001FAC", 0x03001FAC, 64),
    ("DMA1 current block @030023FC", 0x030023FC, 64),
    ("DMA2 PCM ring @030025AC", 0x030025AC, 64),
    ("DMA2 previous/current @030029AC", 0x030029AC, 128),
    ("DMA2 current block @030029FC", 0x030029FC, 64),
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

def dump_memory(label, native, oracle, addr, size) -> None:
    nb = bytes.fromhex(
        native.call(cmd="read_iwram", addr=addr, len=size)["data"])
    ob = bytes.fromhex(
        oracle.call(cmd="read_emu_iwram", addr=addr, len=size)["data"])
    diffs = [i for i in range(size) if nb[i] != ob[i]]
    status = "IDENTICAL" if not diffs else f"DIFF ({len(diffs)} bytes)"
    print(f"  {label}: {status}")
    print(f"    native: {nb.hex(' ')}")
    print(f"    oracle: {ob.hex(' ')}")


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
    ap.add_argument(
        "--timeline", action="store_true",
        help="Print native DMA run/source and oracle source once per frame.")
    ap.add_argument(
        "--find-dma-reset", action="store_true",
        help="After reaching the frame, instruction-step mGBA to DMA2 reset.")
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
            while nf < args.frame or of < args.frame:
                if nf < args.frame:
                    nf = native.call(cmd="step")["frame"]
                if of < args.frame:
                    of = oracle.call(cmd="emu_step")["frame"]
                if args.timeline:
                    ns = native.call(cmd="audio_state")
                    os = oracle.call(cmd="emu_audio_state")
                    oi = oracle.call(cmd="emu_dma_internal")
                    print(
                        f"timeline f={nf}/{of} "
                        f"native_runs={ns.get('dma2', {}).get('runs')} "
                        f"native_src=0x"
                        f"{ns.get('dma2', {}).get('next_source', 0):08x} "
                        f"oracle_src=0x"
                        f"{oi['channels'][2]['nextSource']:08x} "
                        f"native_fifo={ns['fifo_b']['count']}/"
                        f"{ns['fifo_b']['bytes_remaining']} "
                        f"oracle_fifo={os['fifo_b']['count']}/"
                        f"{os['fifo_b']['internalRemaining']}")
            print(f"==> at native@{nf} oracle@{of}")
            if args.find_dma_reset:
                print(
                    "  oracle DMA2 reset event: "
                    f"{oracle.call(cmd='emu_find_dma_source_reset', channel=2, max_steps=5000000)}")
            for r in RANGES:
                dump_range(r[0], native, oracle, r[1], r[2], r[3], r[4])
            for label, addr, size in MEM_RANGES:
                dump_memory(label, native, oracle, addr, size)
            print("  native audio_state:")
            print(f"    {native.call(cmd='audio_state')}")
            print("  oracle emu_audio_state:")
            print(f"    {oracle.call(cmd='emu_audio_state')}")
            print("  native dma_state:")
            print(f"    {native.call(cmd='dma_state')}")
            print("  oracle emu_dma_state:")
            print(f"    {oracle.call(cmd='emu_dma_internal')}")
            print("  native timer_state:")
            print(f"    {native.call(cmd='timer_state')}")
            print("  oracle emu_timer_state:")
            print(f"    {oracle.call(cmd='emu_timer_state')}")
            mmio = native.call(cmd="mmio_cap", count=65536)
            dma_writes = [
                e for e in mmio.get("entries", [])
                if (0x040000BC <= e["addr"] < 0x040000D4) or
                   (0x04000100 <= e["addr"] < 0x04000110)
            ]
            print(
                "  native recent DMA1/2 + timer MMIO writes "
                f"({len(dma_writes)}):")
            for e in dma_writes:
                print(
                    f"    cyc={e['cycle']} pc=0x{e['pc']:08x} "
                    f"addr=0x{e['addr']:08x} size={e['size']} "
                    f"value=0x{e['value']:08x}")
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
