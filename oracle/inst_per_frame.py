#!/usr/bin/env python3
"""inst_per_frame.py — measure how many CPU instructions native and
oracle each execute per PPU frame. The ratio tells us how far our
cycle accounting is from mGBA's.

If native executes more instructions per frame, our cycles-per-
instruction is too LOW (we burn through code faster). If fewer, too
HIGH. Goal: ratio = 1.0 byte-for-byte.

Usage:
    python oracle/inst_per_frame.py --frames 10
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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=10)
    args = ap.parse_args()

    procs = [
        subprocess.Popen(
            [str(BIOS_SMOKE), "--tcp", "19842"],
            cwd=str(ROOT),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
        subprocess.Popen(
            [str(ORACLE), "--bios", "bios/gba_bios.bin", "--port", "19843"],
            cwd=str(ROOT),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
    ]

    try:
        native = JsonClient("127.0.0.1", 19842)
        oracle = JsonClient("127.0.0.1", 19843)
        try:
            print("frame   native_insts   oracle_insts   ratio (n/o)")
            print("-----   ------------   ------------   -----------")
            # Step both by single instruction; record cumulative inst
            # count when each side's PPU frame advances.
            n_inst = 0
            o_inst = 0
            n_at_frame = [0] * (args.frames + 1)  # n_at_frame[f] = native inst count when native reached frame f
            o_at_frame = [0] * (args.frames + 1)
            n_done = 0
            o_done = 0
            while n_done < args.frames or o_done < args.frames:
                if n_done < args.frames:
                    r = native.call(cmd="step_inst")
                    n_inst += 1
                    f = int(r.get("frame", 0))
                    while n_done < f and n_done < args.frames:
                        n_done += 1
                        n_at_frame[n_done] = n_inst
                if o_done < args.frames:
                    r = oracle.call(cmd="emu_step_inst")
                    o_inst += 1
                    f = int(r.get("frame", 0))
                    while o_done < f and o_done < args.frames:
                        o_done += 1
                        o_at_frame[o_done] = o_inst
            for f in range(1, args.frames + 1):
                d_n = n_at_frame[f] - n_at_frame[f - 1]
                d_o = o_at_frame[f] - o_at_frame[f - 1]
                ratio = (d_n / d_o) if d_o else 0.0
                print(f" {f:3d}      {d_n:10d}     {d_o:10d}     {ratio:.4f}")
        finally:
            native.close()
            oracle.close()
    finally:
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
