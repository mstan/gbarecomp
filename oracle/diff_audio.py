#!/usr/bin/env python3
"""Compare BIOS audio samples between native gbarecomp and the mGBA oracle."""

from __future__ import annotations

import argparse
import json
import pathlib
import socket
import struct
import subprocess
import sys
import time
from typing import Optional

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIOS_SMOKE = ROOT / "build" / "bios_smoke.exe"
ORACLE = ROOT / "build" / "oracle" / "gbarecomp_oracle.exe"
BIOS_PATH = "bios/gba_bios.bin"


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
        self.sock.sendall(json.dumps(kwargs).encode("utf-8") + b"\n")
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


def spawn(cmd: list[str], label: str) -> subprocess.Popen:
    print(f"==> spawning {label}: {' '.join(cmd)}")
    return subprocess.Popen(
        cmd, cwd=str(ROOT),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def decode_samples(hex_data: str) -> list[int]:
    blob = bytes.fromhex(hex_data)
    return [v[0] for v in struct.iter_unpack("<h", blob)]


def first_sample_diff(a: list[int], b: list[int]) -> Optional[int]:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return n
    return None


def drain_audio(client: JsonClient, cmd: str) -> tuple[int, int, list[int]]:
    out: list[int] = []
    rate = 0
    generated = 0
    while True:
        resp = client.call(cmd=cmd, max=16384)
        if not resp.get("ok"):
            raise RuntimeError(f"{cmd} failed: {resp}")
        rate = resp["rate"]
        generated = resp["samples_generated"]
        count = resp["count"]
        out.extend(decode_samples(resp["data"]))
        if count < 16384:
            return rate, generated, out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=420)
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    ap.add_argument("--no-spawn", action="store_true")
    args = ap.parse_args()

    procs: list[subprocess.Popen] = []
    native = None
    oracle = None
    try:
        if not args.no_spawn:
            if not BIOS_SMOKE.exists() or not ORACLE.exists():
                print("missing build outputs; run `cmake --build build` first",
                      file=sys.stderr)
                return 1
            procs.append(spawn([str(BIOS_SMOKE), "--tcp", str(args.native_port)],
                               "native"))
            procs.append(spawn([str(ORACLE), "--bios", BIOS_PATH,
                                "--port", str(args.oracle_port)], "oracle"))

        native = JsonClient("127.0.0.1", args.native_port)
        oracle = JsonClient("127.0.0.1", args.oracle_port)

        native_samples: list[int] = []
        oracle_samples: list[int] = []
        native_rate = oracle_rate = 0
        native_generated = oracle_generated = 0

        for frame in range(1, args.frames + 1):
            nf = native.call(cmd="step")["frame"]
            of = oracle.call(cmd="emu_step")["frame"]
            native_rate, native_generated, ns = drain_audio(native, "audio_samples")
            oracle_rate, oracle_generated, os = drain_audio(oracle, "emu_audio_samples")
            native_samples.extend(ns)
            oracle_samples.extend(os)
            if frame == 1 or frame % 30 == 0 or frame == args.frames:
                print(f"frame {frame}: native@{nf} oracle@{of} "
                      f"samples={len(native_samples)}/{len(oracle_samples)}")

        print(f"rates: native={native_rate} oracle={oracle_rate}")
        print(f"generated: native={native_generated} oracle={oracle_generated}")
        print(f"drained: native={len(native_samples)} oracle={len(oracle_samples)}")

        first = first_sample_diff(native_samples, oracle_samples)
        if first is None:
            print("AUDIO: identical")
            return 0

        if first < len(native_samples) and first < len(oracle_samples):
            print(f"AUDIO: first diff sample {first} "
                  f"native={native_samples[first]} oracle={oracle_samples[first]}")
        else:
            print(f"AUDIO: length diff at sample {first}")

        n = min(len(native_samples), len(oracle_samples))
        if n:
            max_abs = 0
            max_abs_idx = -1
            sum_sq = 0
            nonzero_native = 0
            nonzero_oracle = 0
            diff_indices_significant: list[int] = []
            for i, (a, b) in enumerate(zip(native_samples[:n], oracle_samples[:n])):
                d = a - b
                if abs(d) > max_abs:
                    max_abs = abs(d)
                    max_abs_idx = i
                if abs(d) > 100:
                    diff_indices_significant.append(i)
                sum_sq += d * d
                if a:
                    nonzero_native += 1
                if b:
                    nonzero_oracle += 1
            rms = (sum_sq / n) ** 0.5
            print(f"stats: compared={n} max_abs={max_abs}@idx{max_abs_idx} "
                  f"rms={rms:.2f} nonzero={nonzero_native}/{nonzero_oracle}")
            print(f"samples with |diff|>100: {len(diff_indices_significant)} of {n}")
            if diff_indices_significant:
                # Show first 20 of them
                head = diff_indices_significant[:20]
                print(f"  first significant diffs at: {head}")
            # Sample window around first diff (10 before, 20 after).
            lo = max(0, first - 10)
            hi = min(n, first + 20)
            print(f"window samples [{lo}..{hi}):")
            print(f"  idx  native  oracle  diff")
            for i in range(lo, hi):
                d = native_samples[i] - oracle_samples[i]
                marker = "  <-- first" if i == first else ""
                print(f"  {i:6d} {native_samples[i]:7d} "
                      f"{oracle_samples[i]:7d} {d:7d}{marker}")
        return 1
    finally:
        if native:
            native.close()
        if oracle:
            oracle.close()
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    sys.exit(main())
