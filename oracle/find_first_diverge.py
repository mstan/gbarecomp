#!/usr/bin/env python3
"""find_first_diverge.py — step native and oracle one CPU instruction at a
time, comparing PC + registers each step. Reports the first instruction
where they diverge.

Usage:
    python oracle/find_first_diverge.py --max-steps 200000

Both processes are auto-spawned. The script also dumps a small window
of instructions around the divergence so the cause is easy to read.

Caveat: mGBA's emu_step_inst advances one CPU instruction in mGBA's
internal accounting; whether that matches our run_one_cpu_step (which
also handles HALT and IRQ entry) exactly is what we're testing. If
they don't match in basic shape, the divergence is in our step loop
structure, not in interpreter behaviour.
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

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIOS_SMOKE = ROOT / "build" / "bios_smoke.exe"
ORACLE     = ROOT / "build" / "oracle" / "gbarecomp_oracle.exe"


class JsonClient:
    def __init__(self, host: str, port: int):
        deadline = time.time() + 5.0
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

    def call(self, **kwargs) -> dict:
        line = json.dumps(kwargs).encode("utf-8") + b"\n"
        self.sock.sendall(line)
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("peer closed the connection")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def close(self) -> None:
        try:
            self.call(cmd="quit")
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


def regs_to_str(d: dict) -> str:
    parts = [f"r{i}={d.get(f'r{i}', 0):08x}" for i in range(16)]
    parts.append(f"cpsr={d.get('cpsr', 0):08x}")
    return " ".join(parts)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-steps", type=int, default=200000,
                    help="Cap instructions before bailing")
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    args = ap.parse_args()

    procs: list[subprocess.Popen] = []
    try:
        procs.append(subprocess.Popen(
            [str(BIOS_SMOKE), "--tcp", str(args.native_port)],
            cwd=str(ROOT),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        procs.append(subprocess.Popen(
            [str(ORACLE), "--bios", "bios/gba_bios.bin",
             "--port", str(args.oracle_port)],
            cwd=str(ROOT),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

        native = JsonClient("127.0.0.1", args.native_port)
        oracle = JsonClient("127.0.0.1", args.oracle_port)
        try:
            print("Lockstepping...")
            history: list[tuple[int, int, int]] = []  # (step, n_pc, o_pc)
            for i in range(1, args.max_steps + 1):
                n = native.call(cmd="step_inst")
                o = oracle.call(cmd="emu_step_inst")
                if not n.get("ok") or not o.get("ok"):
                    print(f"step {i}: terminated (native_ok={n.get('ok')} "
                          f"oracle_ok={o.get('ok')})")
                    return 2
                n_pc = int(n["pc"])
                # mGBA's r15 includes the pipeline prefetch offset: in
                # ARM state, R15 reads as PC+8; in THUMB, PC+4. Our
                # native stores the raw next-instruction PC. To diff
                # at the same logical PC, normalize mGBA's value
                # downward by the prefetch ahead. We can't tell ARM
                # vs THUMB from the raw int, so we read CPSR's T bit
                # from the oracle response (added to the cmd below).
                o_pc_raw = int(o["pc"])
                o_thumb = bool(o.get("thumb", False))
                o_pc = o_pc_raw - (4 if o_thumb else 8)
                history.append((i, n_pc, o_pc))
                if n_pc != o_pc:
                    print(f"\n*** PC DIVERGED at instruction #{i} ***")
                    print(f"    native pc = 0x{n_pc:08x}")
                    print(f"    oracle pc = 0x{o_pc:08x}")
                    print("\nLast 16 instructions before divergence:")
                    for s, npc, opc in history[-16:]:
                        marker = " " if npc == opc else "*"
                        print(f"  {marker} #{s:>6}  n=0x{npc:08x}  o=0x{opc:08x}")
                    print("\nRegisters at divergence:")
                    nr = native.call(cmd="registers")
                    oR = oracle.call(cmd="emu_registers")
                    print(f"  native: {regs_to_str(nr)}")
                    print(f"  oracle: {regs_to_str(oR)}")
                    # Print field-by-field diff.
                    print("\nRegister differences:")
                    for k in [f"r{j}" for j in range(16)] + ["cpsr"]:
                        nv = nr.get(k, 0)
                        ov = oR.get(k, 0)
                        if nv != ov:
                            print(f"  {k}: native=0x{nv:08x} oracle=0x{ov:08x}")
                    return 0
                if i % 10000 == 0:
                    print(f"  step {i}: pc=0x{n_pc:08x} (still in sync)", flush=True)
            print(f"No divergence in {args.max_steps} instructions.")
            return 0
        finally:
            native.close()
            oracle.close()
    finally:
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    sys.exit(main())
