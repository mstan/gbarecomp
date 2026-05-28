#!/usr/bin/env python3
"""diff_interp.py — recompiler-vs-interpreter differential.

Boots the recompiled runtime (MinishCapRecomp.exe) and the interpreter
oracle (bios_smoke.exe --rom) from reset, replays identical input, and
finds the first frame their IWRAM diverges.

Unlike the mGBA oracle, bios_smoke drives the SAME gba::GbaBus / GbaPpu /
GbaIo / GbaAudio the recompiled runtime uses — there is no second timing
model. So they are byte-identical until a codegen bug, and the first
divergence is exactly the miscompiled code path. Both speak the same
native TCP protocol (step / read_iwram / set_keyinput / registers).

Usage:
    python oracle/diff_interp.py --until 300 --diff-from 1   # lockstep check
    python oracle/diff_interp.py --until 3645 --diff-from 3400
"""

from __future__ import annotations

import argparse
import pathlib
import socket
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from diff_m4a import (JsonClient, spawn, coalesce_diffs,  # noqa: E402
                      NATIVE_EXE, BIOS_PATH, ROM_PATH, PROJ, ROOT)
from diff_intro import held_this_frame, START_KEYINPUT, NONE_KEYINPUT  # noqa: E402

BIOS_SMOKE = ROOT / "build" / "bios_smoke.exe"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--until", type=int, default=300)
    ap.add_argument("--diff-from", type=int, default=1)
    ap.add_argument("--recomp-port", type=int, default=19842)
    ap.add_argument("--interp-port", type=int, default=19844)
    ap.add_argument("--step-timeout", type=float, default=20.0)
    ap.add_argument("--no-spawn", action="store_true")
    ap.add_argument("--values", action="store_true",
                    help="dump native/interp bytes at first divergence")
    args = ap.parse_args()

    procs: list[subprocess.Popen] = []
    try:
        if not args.no_spawn:
            for exe in (NATIVE_EXE, BIOS_SMOKE):
                if not exe.exists():
                    print(f"missing {exe}", file=sys.stderr)
                    return 1
            procs.append(spawn([str(NATIVE_EXE), "--tcp", str(args.recomp_port)],
                               "recomp", PROJ))
            procs.append(spawn([str(BIOS_SMOKE), "--bios", str(BIOS_PATH),
                                "--rom", str(ROM_PATH), "--tcp",
                                str(args.interp_port)], "interp", ROOT))

        rec = JsonClient("127.0.0.1", args.recomp_port)
        itp = JsonClient("127.0.0.1", args.interp_port)
        first_div = None
        try:
            for f in range(1, args.until + 1):
                ki = START_KEYINPUT if held_this_frame(f) else NONE_KEYINPUT
                rec.call(cmd="set_keyinput", value=ki)
                itp.call(cmd="set_keyinput", value=ki)
                itp.call(cmd="step")
                try:
                    rec.call(timeout=args.step_timeout, cmd="step")
                except socket.timeout:
                    print(f"==> FRAME {f}: recomp step TIMED OUT — SPIN.",
                          flush=True)
                    break

                if f < args.diff_from:
                    if f % 500 == 0:
                        print(f"  ...lockstep frame {f}", flush=True)
                    continue

                rb = rec.read_region("read_iwram", 0x03000000, 32 * 1024)
                ib = itp.read_region("read_iwram", 0x03000000, 32 * 1024)
                ndiff = sum(1 for a, b in zip(rb, ib) if a != b)
                if ndiff and first_div is None:
                    first_div = f
                tag = "IDENTICAL" if not ndiff else f"{ndiff} bytes differ"
                print(f"== frame {f}: IWRAM {tag}", flush=True)
                if ndiff:
                    rngs = coalesce_diffs(rb, ib, 0x03000000, max_ranges=12)
                    print("   ranges: " +
                          ", ".join(f"0x{a:08x}+{n}" for a, n in rngs))
                    if args.values:
                        for a, n in rngs[:6]:
                            o = a - 0x03000000
                            print(f"   @0x{a:08x} recomp={rb[o:o+n].hex()} "
                                  f"interp={ib[o:o+n].hex()}")
            if first_div is not None:
                print(f"\n==> FIRST DIVERGENCE at frame {first_div}")
            else:
                print("\n==> no IWRAM divergence in window "
                      f"(recomp == interpreter through frame {args.until})")
        finally:
            rec.close()
            itp.close()
        return 0
    finally:
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    sys.exit(main())
