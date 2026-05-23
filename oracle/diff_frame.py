#!/usr/bin/env python3
"""diff_frame.py — find the first OAM/VRAM/PAL/IWRAM/IO divergence
between gbarecomp's native runtime and the mGBA oracle at a specific
PPU frame, syncing both over TCP.

Usage:
    python oracle/diff_frame.py --frame 60
    python oracle/diff_frame.py --scan 1 30 1    # frames 1..30 step 1

Both processes are auto-spawned if --no-spawn isn't given:
    bios_smoke --tcp 19842
    gbarecomp_oracle --bios bios/gba_bios.bin --port 19843

Sync: each iteration sends `step` (native) and `emu_step` (oracle).
Both advance exactly one PPU frame per call. Per CLAUDE.md SYNC RULES,
PPU frame count is OK as a stop signal (not the primary clock).
"""

from __future__ import annotations

import argparse
import json
import pathlib
import socket
import subprocess
import sys
import time
from typing import Iterable, Optional

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIOS_SMOKE = ROOT / "build" / "bios_smoke.exe"
ORACLE     = ROOT / "build" / "oracle" / "gbarecomp_oracle.exe"
BIOS_PATH  = "bios/gba_bios.bin"

# Region descriptors: (key, display name, byte size, native-cmd, oracle-cmd).
REGIONS = [
    ("oam",   "OAM",      1024,         "read_oam",   "read_emu_oam"),
    ("pal",   "PAL",      1024,         "read_pal",   "read_emu_pal"),
    ("vram",  "VRAM",     96 * 1024,    "read_vram",  "read_emu_vram"),
    ("iwram", "IWRAM",    32 * 1024,    "read_iwram", "read_emu_iwram"),
    ("io",    "IO",       0x400,        "read_io",    "read_emu_io"),
]


class JsonClient:
    """Line-delimited JSON request/response over TCP."""

    def __init__(self, host: str, port: int):
        # Retry briefly on connection — peer may still be binding.
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

    def read_region_chunked(self, cmd: str, size: int) -> bytes:
        # Oracle / native protocol cap is 8192 chars/line; 4096-byte
        # chunks fit comfortably (4 KB hex = 8 KB).
        chunk = 4096
        out = bytearray()
        for off in range(0, size, chunk):
            n = min(chunk, size - off)
            resp = self.call(cmd=cmd, addr=off, len=n)
            if not resp.get("ok"):
                raise RuntimeError(f"{cmd} @{off}: {resp}")
            out += bytes.fromhex(resp["data"])
        return bytes(out)


def spawn(cmd: list, label: str) -> subprocess.Popen:
    print(f"==> spawning {label}: {' '.join(cmd)}")
    return subprocess.Popen(
        cmd, cwd=str(ROOT),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def find_first_diff(a: bytes, b: bytes) -> Optional[int]:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return n
    return None


def diff_one_frame(frame: int, native: JsonClient, oracle: JsonClient) -> None:
    # Ensure both are caught up to `frame`.
    nf = native.call(cmd="frame")["frame"]
    of = oracle.call(cmd="frame")["frame"]
    if nf > frame or of > frame:
        # If either ran past the target (shouldn't happen in scan mode
        # if we only step monotonically), just print and bail.
        print(f"==> frame {frame}: native@{nf} oracle@{of} — out of sync, skipping")
        return
    print(f"==> Frame {frame}: stepping ({frame - nf} native, {frame - of} oracle)...",
          flush=True)
    while nf < frame:
        nf = native.call(cmd="step")["frame"]
    while of < frame:
        of = oracle.call(cmd="emu_step")["frame"]

    native_fb = native.call(cmd="screenshot")
    oracle_fb = oracle.call(cmd="emu_screenshot")
    if not native_fb.get("ok"):
        raise RuntimeError(f"native screenshot failed: {native_fb}")
    if not oracle_fb.get("ok"):
        raise RuntimeError(f"oracle screenshot failed: {oracle_fb}")
    nfb = bytes.fromhex(native_fb["data"])
    ofb = bytes.fromhex(oracle_fb["data"])
    first_fb = find_first_diff(nfb, ofb)
    if first_fb is None:
        print("    FRAMEBUFFER: identical")
    else:
        pixel = first_fb // 3
        chan = first_fb % 3
        x = pixel % 240
        y = pixel // 240
        print(f"    FRAMEBUFFER: first diff @({x},{y}) channel={chan} "
              f"native=0x{nfb[first_fb]:02x} oracle=0x{ofb[first_fb]:02x}")

    for key, name, size, ncmd, ocmd in REGIONS:
        native_blob = native.read_region_chunked(ncmd, size)
        oracle_blob = oracle.read_region_chunked(ocmd, size)
        first = find_first_diff(native_blob, oracle_blob)
        if first is None:
            print(f"    {name}: identical")
        else:
            nb = native_blob[first]
            ob = oracle_blob[first]
            print(f"    {name}: first diff @0x{first:05x} "
                  f"native=0x{nb:02x} oracle=0x{ob:02x}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frame", type=int, default=60)
    ap.add_argument("--scan", nargs=3, type=int, metavar=("START", "END", "STEP"))
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    ap.add_argument("--no-spawn", action="store_true",
                    help="Assume both processes are already running")
    args = ap.parse_args()

    procs: list[subprocess.Popen] = []
    try:
        if not args.no_spawn:
            if not BIOS_SMOKE.exists():
                print(f"missing {BIOS_SMOKE} (cmake --build build first)",
                      file=sys.stderr)
                return 1
            if not ORACLE.exists():
                print(f"missing {ORACLE} (build with -DGBARECOMP_BUILD_ORACLE=ON)",
                      file=sys.stderr)
                return 1
            procs.append(spawn(
                [str(BIOS_SMOKE), "--tcp", str(args.native_port)],
                "native"))
            procs.append(spawn(
                [str(ORACLE), "--bios", BIOS_PATH, "--port", str(args.oracle_port)],
                "oracle"))

        native = JsonClient("127.0.0.1", args.native_port)
        oracle = JsonClient("127.0.0.1", args.oracle_port)
        try:
            if args.scan:
                start, end, step = args.scan
                frames: Iterable[int] = range(start, end + 1, step)
            else:
                frames = [args.frame]
            for f in frames:
                diff_one_frame(f, native, oracle)
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
