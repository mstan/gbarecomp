#!/usr/bin/env python3
"""diff_cart.py — same-instant native-vs-oracle comparison for a CARTRIDGE run.

find_first_diverge.py compares BIOS execution: it drives bios_smoke and never
passes --rom, so it cannot say anything about a game. This is its cartridge
sibling. Both sides boot the same ROM, are advanced to the same VBlank count
(hardware event, per the SYNC RULES — never a raw frame index), and then their
PAL / VRAM / OAM / IO are compared byte-for-byte.

The point is to separate two failures that look identical on screen:

    regions match, pixels differ  -> our COMPOSITOR is wrong
    regions differ                -> guest EXECUTION diverged first, and the
                                     picture is downstream of that

Usage:
    python oracle/diff_cart.py --rom path/to/game.gba --frame 700
    python oracle/diff_cart.py --rom game.gba --scan 100 1200 100

`--scan lo hi step` walks VBlank counts and reports the first one that differs,
which is usually what you actually want: the earliest divergence is the only one
with a root cause.

IMPORTANT for RTC carts (Boktai, Pokémon Ruby/Sapphire/Emerald, ...): the two
processes read the clock independently, so a bare comparison can report a
"divergence" that is only a difference in wall time. Pin ours with
RECOMP_RTC_EPOCH and be aware the oracle side still follows host time; treat any
day/night-dependent difference as uncontrolled unless both clocks are fixed.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import socket
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "oracle"))
from recomp_paths import exe_name, recomp_exe  # noqa: E402

ORACLE = ROOT / "build" / "oracle" / exe_name("gbarecomp_oracle")
BIOS = ROOT / "bios" / "gba_bios.bin"

# Region name -> (command pair, base address, byte length).
REGIONS = [
    ("PAL",   "read_pal",   "read_emu_pal",   0x05000000, 0x400),
    ("OAM",   "read_oam",   "read_emu_oam",   0x07000000, 0x400),
    ("VRAM",  "read_vram",  "read_emu_vram",  0x06000000, 0x18000),
    ("IWRAM", "read_iwram", "read_emu_iwram", 0x03000000, 0x8000),
]

# Display / blend / window registers, reported on a mismatch because they are
# what usually explains a pixel difference when the regions agree.
IO_REGS = [
    (0x04000000, "DISPCNT"), (0x04000008, "BG0CNT"), (0x0400000A, "BG1CNT"),
    (0x0400000C, "BG2CNT"), (0x0400000E, "BG3CNT"), (0x04000048, "WININ"),
    (0x0400004A, "WINOUT"), (0x04000050, "BLDCNT"), (0x04000052, "BLDALPHA"),
    (0x04000054, "BLDY"),
]


class Client:
    def __init__(self, port: int, timeout: float = 30.0):
        deadline = time.time() + timeout
        self.sock = None
        last: Exception | None = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2.0)
                break
            except OSError as e:
                last = e
                time.sleep(0.2)
        if self.sock is None:
            raise RuntimeError(f"cannot reach 127.0.0.1:{port}: {last}")
        self.sock.settimeout(600.0)
        self.buf = b""

    def call(self, **kw) -> dict:
        self.sock.sendall(json.dumps(kw).encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise RuntimeError("peer closed the connection")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode())

    def close(self) -> None:
        for fn in (lambda: self.call(cmd="quit"), self.sock.close):
            try:
                fn()
            except Exception:
                pass


def hexbytes(d: str) -> bytes:
    return bytes.fromhex(d)


def read_hw(cli: Client, cmd: str, addr: int) -> int:
    r = cli.call(cmd=cmd, addr=addr, len=2)
    d = r["data"]
    return int(d[0:2], 16) | (int(d[2:4], 16) << 8)


def compare(native: Client, oracle: Client) -> list[tuple[str, int, int]]:
    """Return [(region, differing_bytes, total)] for regions that differ."""
    out = []
    for name, ncmd, ocmd, base, ln in REGIONS:
        a = hexbytes(native.call(cmd=ncmd, addr=base, len=ln)["data"])
        b = hexbytes(oracle.call(cmd=ocmd, addr=base, len=ln)["data"])
        n = sum(1 for i in range(min(len(a), len(b))) if a[i] != b[i])
        if n:
            out.append((name, n, ln))
    return out


def advance(native: Client, oracle: Client, by: int) -> None:
    native.call(cmd="run_frames", n=by)
    for _ in range(by):
        oracle.call(cmd="emu_step_to_vblank")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--bios", default=str(BIOS))
    ap.add_argument("--frame", type=int, help="compare once at this VBlank count")
    ap.add_argument("--scan", type=int, nargs=3, metavar=("LO", "HI", "STEP"),
                    help="walk VBlank counts and stop at the first difference")
    ap.add_argument("--native-exe", default=None,
                    help="game executable (default: resolved via recomp_paths)")
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    a = ap.parse_args()

    if not a.frame and not a.scan:
        a.frame = 600
    native_exe = pathlib.Path(a.native_exe) if a.native_exe else recomp_exe(ROOT)
    for p, what in ((ORACLE, "oracle binary"), (native_exe, "game executable"),
                    (pathlib.Path(a.bios), "BIOS"), (pathlib.Path(a.rom), "ROM")):
        if not p.exists():
            print(f"missing {what}: {p}")
            return 2

    procs = []
    n = o = None
    try:
        procs.append(subprocess.Popen(
            [str(native_exe), "--bios", a.bios, "--rom", a.rom,
             "--tcp", str(a.native_port)],
            cwd=str(native_exe.parent.parent), env=dict(os.environ),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        procs.append(subprocess.Popen(
            [str(ORACLE), "--bios", a.bios, "--rom", a.rom,
             "--port", str(a.oracle_port)],
            cwd=str(ROOT),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        n = Client(a.native_port)
        o = Client(a.oracle_port)

        targets = ([a.frame] if a.frame
                   else list(range(a.scan[0], a.scan[1] + 1, a.scan[2])))
        cur = 0
        for want in targets:
            advance(n, o, want - cur)
            cur = want
            diffs = compare(n, o)
            if not diffs:
                print(f"vblank {want:>6}: identical "
                      f"({', '.join(r[0] for r in REGIONS)})")
                continue
            print(f"\nvblank {want}: DIVERGED")
            for name, cnt, total in diffs:
                print(f"    {name}: {cnt} / {total} bytes differ")
            print("  display/blend/window registers at this instant:")
            for addr, label in IO_REGS:
                nv = read_hw(n, "read_io", addr)
                ov = read_hw(o, "read_emu_io", addr)
                flag = "  <-- differs" if nv != ov else ""
                print(f"    {label:9} ours=0x{nv:04X} oracle=0x{ov:04X}{flag}")
            names = {d[0] for d in diffs}
            if names <= {"PAL"}:
                print("\n  Only PAL differs — suspect palette writes, not geometry.")
            elif "OAM" in names or "VRAM" in names:
                print("\n  VRAM/OAM differ, so guest EXECUTION diverged before "
                      "rendering.\n  The compositor is not implicated by this "
                      "result.")
            return 1
        print("\nno divergence found across the requested points")
        return 0
    finally:
        for c in (n, o):
            if c:
                c.close()
        for p in procs:
            p.terminate()
            try:
                p.wait(timeout=5)
            except Exception:
                p.kill()


if __name__ == "__main__":
    sys.exit(main())
