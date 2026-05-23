#!/usr/bin/env python3
"""diff_frame.py — find the first OAM/VRAM/PAL/IWRAM divergence between
gbarecomp's native runtime and the mGBA-backed oracle at a specific
PPU frame.

Usage:
    python oracle/diff_frame.py --frame 60
    python oracle/diff_frame.py --scan 10 240 30   # frames 10,40,70,...,240

The oracle process must already be running:
    ./build/oracle/gbarecomp_oracle.exe --bios bios/gba_bios.bin

This script:
  1. Runs bios_smoke --frames N --snapshot tmp/native, capturing native
     OAM/VRAM/PAL/IWRAM as raw blobs.
  2. Connects to the oracle, steps N frames, reads the same regions.
  3. Diffs byte-for-byte; reports the first differing offset per region.

We sync via "frame count" rather than wall-clock — both runtimes count
PPU frames the same way. Per CLAUDE.md SYNC RULES, frame index is OK
when used as a stop signal (not as a primary clock).
"""

from __future__ import annotations

import argparse
import os
import pathlib
import socket
import struct  # noqa: F401  (kept for future binary structs)
import subprocess
import sys
import json
from typing import Iterable, Optional

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIOS_SMOKE = ROOT / "build" / "bios_smoke.exe"
SNAPSHOT_DIR = ROOT / "build" / "tmp_snapshot"

# Region descriptors: (region key, hex-readable name, byte size, oracle
# command). Order = priority. OAM first because that's where we've
# already observed divergence at frame 180.
REGIONS = [
    ("oam",   "OAM",      1024,         "read_emu_oam"),
    ("pal",   "PAL",      1024,         "read_emu_pal"),
    ("vram",  "VRAM",     96 * 1024,    "read_emu_vram"),
    ("iwram", "IWRAM",    32 * 1024,    "read_emu_iwram"),
    ("io",    "IO",       0x400,        "read_emu_io"),
]


def run_native(frame: int, prefix: pathlib.Path) -> None:
    """Drive bios_smoke to `frame`, write region blobs at `prefix`."""
    prefix.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(BIOS_SMOKE),
        "--frames", str(frame),
        "--quiet",
        "--snapshot", str(prefix),
    ]
    r = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
    if r.returncode != 0:
        print(f"native: bios_smoke exit {r.returncode}", file=sys.stderr)
        print(r.stderr, file=sys.stderr)
        sys.exit(2)


def read_native_blobs(prefix: pathlib.Path) -> dict[str, bytes]:
    blobs = {}
    for key, _name, size, _cmd in REGIONS:
        path = prefix.with_suffix(prefix.suffix + f".{key}")
        # snapshot wrote literally "<prefix>.<key>" so reconstruct that.
        path = pathlib.Path(str(prefix) + f".{key}")
        if not path.exists():
            print(f"native: missing {path}", file=sys.stderr)
            sys.exit(2)
        b = path.read_bytes()
        if len(b) != size:
            print(f"native: {path} is {len(b)} bytes, expected {size}",
                  file=sys.stderr)
        blobs[key] = b
    return blobs


class Oracle:
    """Line-delimited JSON client to gbarecomp_oracle."""

    def __init__(self, host: str = "127.0.0.1", port: int = 19843):
        self.sock = socket.create_connection((host, port))
        self.buf = b""

    def call(self, **kwargs) -> dict:
        line = json.dumps(kwargs).encode("utf-8") + b"\n"
        self.sock.sendall(line)
        # Read until newline.
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("oracle closed the connection")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def close(self) -> None:
        try:
            self.call(cmd="quit")
        except Exception:
            pass
        self.sock.close()

    def step_to_frame(self, target_frame: int) -> int:
        """Call emu_step until frame counter reaches target. Returns actual frame."""
        last = 0
        while True:
            resp = self.call(cmd="emu_step")
            if not resp.get("ok"):
                raise RuntimeError(f"oracle step failed: {resp}")
            last = int(resp["frame"])
            if last >= target_frame:
                return last

    def read_region(self, region_cmd: str, size: int) -> bytes:
        # Pull in 16 KB chunks to keep request lines small. The oracle
        # accepts up to 8192 chars per command; a 16 KB read needs
        # 32 KB of hex which is too long. Use 4 KB chunks instead.
        chunk = 4096
        out = bytearray()
        for off in range(0, size, chunk):
            n = min(chunk, size - off)
            resp = self.call(cmd=region_cmd, addr=off, len=n)
            if not resp.get("ok"):
                raise RuntimeError(f"oracle {region_cmd} @{off} failed: {resp}")
            out += bytes.fromhex(resp["data"])
        return bytes(out)


def find_first_diff(a: bytes, b: bytes) -> Optional[int]:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return n
    return None


def diff_one_frame(frame: int, oracle: Oracle) -> dict:
    """Run native + oracle to `frame`, return diff summary."""
    prefix = SNAPSHOT_DIR / f"frame_{frame:04d}"
    print(f"==> Frame {frame}: running native...", flush=True)
    run_native(frame, prefix)
    native = read_native_blobs(prefix)

    # Oracle steps from wherever it is; if it's ahead, we'd have to
    # reset. For now we assume the harness either resets first or runs
    # increasing frames in order.
    cur = oracle.call(cmd="frame")
    if cur["frame"] > frame:
        print(f"    oracle ahead ({cur['frame']} > {frame}); resetting",
              flush=True)
        oracle.call(cmd="emu_reset")
    print(f"    oracle stepping to frame {frame}...", flush=True)
    oracle.step_to_frame(frame)

    summary = {"frame": frame, "regions": {}}
    for key, name, size, cmd in REGIONS:
        print(f"    {name}: reading oracle ({size} bytes)...", flush=True)
        oracle_blob = oracle.read_region(cmd, size)
        first = find_first_diff(native[key], oracle_blob)
        summary["regions"][key] = {
            "size": size,
            "first_diff": first,
            "native_byte": native[key][first] if first is not None else None,
            "oracle_byte": oracle_blob[first] if first is not None else None,
        }
        if first is None:
            print(f"      {name}: identical")
        else:
            n = native[key][first]
            o = oracle_blob[first]
            print(f"      {name}: first diff @0x{first:05x} "
                  f"native=0x{n:02x} oracle=0x{o:02x}")
    return summary


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frame", type=int, default=60,
                    help="Single frame to diff at")
    ap.add_argument("--scan", nargs=3, type=int, metavar=("START", "END", "STEP"),
                    help="Scan a range of frames")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19843)
    args = ap.parse_args()

    if not BIOS_SMOKE.exists():
        print(f"missing {BIOS_SMOKE} (cmake --build build first)",
              file=sys.stderr)
        return 1

    try:
        oracle = Oracle(args.host, args.port)
    except OSError as e:
        print(f"can't reach oracle at {args.host}:{args.port}: {e}",
              file=sys.stderr)
        print("start it with: build/oracle/gbarecomp_oracle.exe --bios bios/gba_bios.bin",
              file=sys.stderr)
        return 1

    try:
        if args.scan:
            start, end, step = args.scan
            frames: Iterable[int] = range(start, end + 1, step)
        else:
            frames = [args.frame]
        for f in frames:
            diff_one_frame(f, oracle)
    finally:
        oracle.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
