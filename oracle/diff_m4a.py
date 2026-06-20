#!/usr/bin/env python3
"""diff_m4a.py — sync the mGBA oracle to a native gbarecomp savestate,
hold an input, and find the first frame the M4A sound state diverges.

Built for MC-HP-002: the recomp's M4A sequencer spins at guest PC
0x08004286 walking a null song-0 track during a transition. This
harness injects a native savestate (RAM + CPU regs) into mGBA, drives
the same held input on both, and steps frame-by-frame diffing IWRAM —
so we can see whether hardware (mGBA) ever processes the same track
with selector 0, and at which frame native first diverges.

Unlike diff_frame.py (which syncs from boot via stepping), this syncs
mid-game by *injecting* state. mGBA's peripheral micro-state isn't
captured by the GBAS savestate, so frame-1 may show benign IO/timer
diffs; we focus on the M4A struct region in IWRAM.

Usage:
    python oracle/diff_m4a.py --state <path> --frames 60 --hold up

Both processes are auto-spawned unless --no-spawn is given.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import socket
import subprocess
import sys
import time
from typing import Optional

ROOT = pathlib.Path(__file__).resolve().parent.parent          # gbarecomp/
import recomp_paths as _rp
PROJ = _rp.game_dir(ROOT)
NATIVE_EXE = _rp.recomp_exe(ROOT)
ORACLE     = ROOT / "build" / "oracle" / "gbarecomp_oracle.exe"
BIOS_PATH  = ROOT / "bios" / "gba_bios.bin"
ROM_PATH   = PROJ / "roms" / "minishcap_usa.gba"
DEF_STATE  = PROJ / "roms" / "minishcap_usa.state3"

# Regions injected into mGBA. (native-read-cmd, bus base, size, width).
# VRAM/PAL/OAM are display-only and not needed for M4A control flow.
INJECT = [
    ("read_iwram", 0x03000000, 32 * 1024, 8),
    ("read_ewram", 0x02000000, 256 * 1024, 8),
    ("read_io",    0x04000000, 0x400, 16),
]

# IO halfword offsets to NOT inject (side effects on write): sound
# FIFO append (0xA0-0xA7), DMA enable halfwords (CNT_H), POSTFLG/HALTCNT.
IO_SKIP = set(range(0xA0, 0xA8)) | {0xBA, 0xC6, 0xD2, 0xDE, 0x300, 0x301}

# mGBA setKeys mask bits (active-high), GBA KEYINPUT bit order.
MGBA_KEYS = {"a": 1, "b": 2, "select": 4, "start": 8, "right": 16,
             "left": 32, "up": 64, "down": 128, "r": 256, "l": 512}

# M4A track struct under investigation and the fields of interest.
M4A_STRUCT = 0x030018D0
M4A_LEN    = 0x80


class JsonClient:
    """Line-delimited JSON request/response over TCP."""

    def __init__(self, host: str, port: int):
        deadline = time.time() + 8.0
        last_err: Optional[Exception] = None
        self.sock = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection((host, port), timeout=2.0)
                break
            except OSError as e:
                last_err = e
                time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError(f"can't reach {host}:{port}: {last_err}")
        self.sock.settimeout(None)
        self.buf = b""

    def call(self, timeout: Optional[float] = None, **kwargs) -> dict:
        line = json.dumps(kwargs).encode("utf-8") + b"\n"
        self.sock.sendall(line)
        self.sock.settimeout(timeout)
        try:
            while b"\n" not in self.buf:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("peer closed the connection")
                self.buf += chunk
        finally:
            self.sock.settimeout(None)
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def close(self) -> None:
        try:
            self.call(timeout=2.0, cmd="quit")
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass

    def read_region(self, cmd: str, base: int, size: int) -> bytes:
        chunk = 1024
        out = bytearray()
        for off in range(0, size, chunk):
            n = min(chunk, size - off)
            resp = self.call(cmd=cmd, addr=base + off, len=n)
            if not resp.get("ok"):
                raise RuntimeError(f"{cmd} @{base + off:#x}: {resp}")
            out += bytes.fromhex(resp["data"])
        return bytes(out)


def spawn(cmd: list, label: str, cwd: pathlib.Path) -> subprocess.Popen:
    print(f"==> spawning {label} (cwd={cwd}): {' '.join(cmd)}", flush=True)
    return subprocess.Popen(cmd, cwd=str(cwd),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def inject_state(native: JsonClient, oracle: JsonClient, state_path: str) -> None:
    print(f"==> loading native savestate: {state_path}", flush=True)
    r = native.call(cmd="savestate_load", path=state_path)
    if not r.get("ok"):
        raise RuntimeError(f"savestate_load failed: {r}")

    oracle.call(cmd="emu_reset")

    for ncmd, base, size, width in INJECT:
        blob = native.read_region(ncmd, base, size)
        if width == 16:
            # Inject halfword-wise, skipping side-effecting IO regs.
            for off in range(0, size, 2):
                if off in IO_SKIP:
                    continue
                hw = blob[off:off + 2]
                oracle.call(cmd="emu_write", addr=base + off,
                            data=hw.hex(), width=16)
        else:
            chunk = 1024
            for off in range(0, size, chunk):
                seg = blob[off:off + chunk]
                oracle.call(cmd="emu_write", addr=base + off,
                            data=seg.hex(), width=8)
        print(f"    injected {ncmd} ({size} bytes)", flush=True)

    regs = native.call(cmd="registers")
    if not regs.get("ok"):
        raise RuntimeError(f"registers failed: {regs}")
    reg_args = {f"r{i}": regs[f"r{i}"] for i in range(16)}
    reg_args["cpsr"] = regs["cpsr"]
    oracle.call(cmd="emu_set_regs", **reg_args)
    print(f"    injected regs: pc=0x{regs['r15']:08x} cpsr=0x{regs['cpsr']:08x}",
          flush=True)

    # The savestate PC sits inside the BIOS VBlankIntrWait halt-check
    # loop, which logically holds IME=1 (set earlier in the routine).
    # Savestates can capture a transient IME=0 window; mGBA resuming
    # mid-loop with IME=0 would never deliver the pending VBlank IRQ and
    # would spin forever. Restore the value the loop assumes.
    ime = native.call(cmd="read_io", addr=0x04000208, len=2)["data"]
    print(f"    native IME = 0x{ime} -> forcing oracle IME=1", flush=True)
    oracle.call(cmd="emu_write", addr=0x04000208, data="0100", width=16)


def m4a_fields(blob: bytes) -> dict:
    """Pull the fields the spin depends on out of the 0x030018D0 blob."""
    def u8(o):  return blob[o]
    def u16(o): return blob[o] | (blob[o + 1] << 8)
    def u32(o): return (blob[o] | (blob[o + 1] << 8) |
                        (blob[o + 2] << 16) | (blob[o + 3] << 24))
    return {
        "sel@12": u16(0x12),
        "f58": u8(0x58),
        "f59": u8(0x59),
        "cmdptr@5c": u32(0x5c),
    }


def coalesce_diffs(a: bytes, b: bytes, base: int, max_ranges: int = 8):
    n = min(len(a), len(b))
    ranges = []
    i = 0
    while i < n:
        if a[i] != b[i]:
            start = i
            while i < n and a[i] != b[i]:
                i += 1
            ranges.append((base + start, i - start))
            if len(ranges) >= max_ranges:
                break
        else:
            i += 1
    return ranges


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default=str(DEF_STATE))
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--hold", default="up",
                    help="comma-separated keys to hold (e.g. up,a)")
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    ap.add_argument("--step-timeout", type=float, default=10.0,
                    help="seconds before a native step is treated as a spin")
    ap.add_argument("--no-spawn", action="store_true")
    args = ap.parse_args()

    keys = [k.strip().lower() for k in args.hold.split(",") if k.strip()]
    mgba_mask = 0
    keyinput = 0x3FF  # active-low; clear a bit to press
    for k in keys:
        if k not in MGBA_KEYS:
            print(f"unknown key {k!r}", file=sys.stderr)
            return 1
        bit = MGBA_KEYS[k]
        mgba_mask |= bit
        keyinput &= ~bit
    print(f"==> holding {keys}: native keyinput=0x{keyinput:03x} "
          f"oracle mask=0x{mgba_mask:x}", flush=True)

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
        spun = False
        try:
            inject_state(native, oracle, args.state)

            # Sync check: the injected M4A region must match exactly.
            nb = native.read_region("read_iwram", M4A_STRUCT, M4A_LEN)
            ob = oracle.read_region("read_emu_iwram", M4A_STRUCT, M4A_LEN)
            if nb == ob:
                print("==> sync check: M4A region identical after injection")
            else:
                rngs = coalesce_diffs(nb, ob, M4A_STRUCT)
                print(f"==> sync check FAILED: M4A region differs {rngs}")
            print(f"    native {m4a_fields(nb)}")
            print(f"    oracle {m4a_fields(ob)}")

            native.call(cmd="set_keyinput", value=keyinput)
            oracle.call(cmd="emu_set_keys", keys=mgba_mask)

            # Oracle liveness probe (VRAM-independent): does mGBA actually
            # execute the game loop after injection? Watch PC, VCOUNT, IRQ
            # flags, and the oracle's own IWRAM frame-to-frame delta.
            print("==> oracle liveness probe:")
            for who, cl, cmd in (("native", native, "read_iwram"),
                                 ("oracle", oracle, "read_emu_iwram")):
                hp = cl.call(cmd=cmd, addr=0x03007FFC, len=4)["data"]
                print(f"   {who} user-IRQ-handler [0x03007FFC] = {hp}")
            hpm = oracle.call(cmd="read_emu_iwram", addr=0x03FFFFFC, len=4)
            print(f"   oracle mirror [0x03FFFFFC] = {hpm}")
            prev_oiw = oracle.read_region("read_emu_iwram", 0x03000000, 32 * 1024)
            for p in range(5):
                oracle.call(cmd="emu_step")
                pc = oracle.call(cmd="emu_pc").get("pc")
                irq = oracle.call(cmd="emu_irq_state")
                ppu = oracle.call(cmd="emu_ppu_state")
                oiw = oracle.read_region("read_emu_iwram", 0x03000000, 32 * 1024)
                delta = sum(1 for a, b in zip(prev_oiw, oiw) if a != b)
                prev_oiw = oiw
                print(f"   step {p}: pc=0x{pc:08x} vcount={ppu.get('vcount')} "
                      f"ie=0x{irq.get('ie'):x} if=0x{irq.get('if'):x} "
                      f"ime={irq.get('ime')} | IWRAM self-delta={delta}",
                      flush=True)

            shot_o0 = shot_n0 = None
            for f in range(1, args.frames + 1):
                of = oracle.call(cmd="emu_step")
                try:
                    nf = native.call(timeout=args.step_timeout, cmd="step")
                except socket.timeout:
                    print(f"==> FRAME {f}: native `step` TIMED OUT "
                          f"({args.step_timeout}s) — SPIN. oracle reached "
                          f"frame {of.get('frame')}.", flush=True)
                    spun = True
                    break

                # Liveness: confirm both screens move under held input. If
                # the oracle's own screen never changes, its game loop isn't
                # executing and the f59 "freeze" would be an injection
                # artifact rather than real hardware behavior.
                if f == 2:
                    shot_o0 = oracle.call(cmd="emu_screenshot").get("data")
                    shot_n0 = native.call(cmd="screenshot").get("data")
                if f == args.frames or f == 30:
                    so = oracle.call(cmd="emu_screenshot").get("data")
                    sn = native.call(cmd="screenshot").get("data")
                    if shot_o0 is not None:
                        od = sum(1 for a, b in zip(shot_o0, so) if a != b)
                        nd = sum(1 for a, b in zip(shot_n0, sn) if a != b)
                        print(f"   LIVENESS f2->f{f}: oracle screen changed "
                              f"{od} nibbles, native {nd}", flush=True)

                niw = native.read_region("read_iwram", 0x03000000, 32 * 1024)
                oiw = oracle.read_region("read_emu_iwram", 0x03000000, 32 * 1024)
                m4a_n = m4a_fields(niw[M4A_STRUCT - 0x03000000:][:M4A_LEN])
                m4a_o = m4a_fields(oiw[M4A_STRUCT - 0x03000000:][:M4A_LEN])
                rngs = coalesce_diffs(niw, oiw, 0x03000000)
                tag = "" if rngs else " (IWRAM identical)"
                print(f"== frame {f}{tag}", flush=True)
                if m4a_n != m4a_o:
                    print(f"   M4A DIVERGE  native={m4a_n}")
                    print(f"                oracle={m4a_o}")
                else:
                    print(f"   M4A ok       {m4a_n}")
                if rngs:
                    pretty = ", ".join(f"0x{a:08x}+{n}" for a, n in rngs)
                    print(f"   IWRAM diffs: {pretty}")
        finally:
            native.close()
            oracle.close()
        return 0 if not spun else 2
    finally:
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    sys.exit(main())
