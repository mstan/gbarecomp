#!/usr/bin/env python3
"""diff_intro.py — boot native gbarecomp and the mGBA oracle from reset,
replay an identical input script, and find the first frame their state
diverges, leading up to the MC-HP-002 M4A spin.

No state injection: both run their own BIOS from reset (the BIOS intro is
bit-identical per the Phase 2.7 gate) and the same ROM with the same
inputs, so they stay in lockstep until the first recompiler bug. The
intro "stained-glass" freeze reproduces on native at ~frame 3645 when
START is pulsed through the title screen.

Usage:
    python oracle/diff_intro.py --until 3000          # checkpoint: in sync?
    python oracle/diff_intro.py --until 3645 --diff-from 3400
"""

from __future__ import annotations

import argparse
import pathlib
import socket
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from diff_m4a import (JsonClient, spawn, coalesce_diffs, m4a_fields,  # noqa: E402
                      NATIVE_EXE, ORACLE, BIOS_PATH, ROM_PATH, PROJ, ROOT,
                      M4A_STRUCT, M4A_LEN)

START_KEYINPUT = 0x3F7   # active-low, START pressed (bit3 clear)
START_MGBA     = 8       # mGBA setKeys mask, START
NONE_KEYINPUT  = 0x3FF
NONE_MGBA      = 0


def held_this_frame(f: int) -> bool:
    """Identical schedule to the native repro probe: pulse START."""
    return f >= 200 and (f // 6) % 6 == 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--until", type=int, default=3645)
    ap.add_argument("--diff-from", type=int, default=3400)
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    ap.add_argument("--step-timeout", type=float, default=15.0)
    ap.add_argument("--no-spawn", action="store_true")
    args = ap.parse_args()

    procs: list[subprocess.Popen] = []
    try:
        if not args.no_spawn:
            for exe in (NATIVE_EXE, ORACLE):
                if not exe.exists():
                    print(f"missing {exe}", file=sys.stderr)
                    return 1
            procs.append(spawn([str(NATIVE_EXE), "--tcp", str(args.native_port)],
                               "native", PROJ))
            procs.append(spawn([str(ORACLE), "--bios", str(BIOS_PATH),
                                "--rom", str(ROM_PATH),
                                "--port", str(args.oracle_port)], "oracle", ROOT))

        native = JsonClient("127.0.0.1", args.native_port)
        oracle = JsonClient("127.0.0.1", args.oracle_port)
        first_div = None
        try:
            for f in range(1, args.until + 1):
                held = held_this_frame(f)
                native.call(cmd="set_keyinput",
                            value=START_KEYINPUT if held else NONE_KEYINPUT)
                oracle.call(cmd="emu_set_keys",
                            keys=START_MGBA if held else NONE_MGBA)
                oracle.call(cmd="emu_step")
                try:
                    native.call(timeout=args.step_timeout, cmd="step")
                except socket.timeout:
                    print(f"==> FRAME {f}: native step TIMED OUT — SPIN. "
                          f"(oracle continues fine)", flush=True)
                    break

                if f < args.diff_from:
                    if f % 500 == 0:
                        print(f"  ...lockstep frame {f}", flush=True)
                    continue

                niw = native.read_region("read_iwram", 0x03000000, 32 * 1024)
                oiw = oracle.read_region("read_emu_iwram", 0x03000000, 32 * 1024)
                rngs = coalesce_diffs(niw, oiw, 0x03000000, max_ranges=12)
                ndiff = sum(1 for a, b in zip(niw, oiw) if a != b)
                m4n = m4a_fields(niw[M4A_STRUCT - 0x03000000:][:M4A_LEN])
                m4o = m4a_fields(oiw[M4A_STRUCT - 0x03000000:][:M4A_LEN])
                if ndiff and first_div is None:
                    first_div = f
                tag = "IDENTICAL" if not ndiff else f"{ndiff} bytes differ"
                print(f"== frame {f}: IWRAM {tag}", flush=True)
                if ndiff:
                    pretty = ", ".join(f"0x{a:08x}+{n}" for a, n in rngs)
                    print(f"   ranges: {pretty}")
                    if m4n != m4o:
                        print(f"   M4A native={m4n}")
                        print(f"       oracle={m4o}")
            if first_div is not None:
                print(f"\n==> FIRST IWRAM DIVERGENCE at frame {first_div}")
            else:
                print("\n==> no IWRAM divergence observed in window")
        finally:
            native.close()
            oracle.close()
        return 0
    finally:
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    sys.exit(main())
