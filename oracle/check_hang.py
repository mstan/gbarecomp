#!/usr/bin/env python3
"""check_hang.py — does the recomp still HANG going through the intro? (MC-HP-002)

Recomp-only, fresh boot, no insn-trace (fast). Drives the same START-pulse intro
schedule as diff_intro.py and steps frame by frame with a timeout. A real hang is
an infinite loop inside one step_frame → the `step` never returns → socket
timeout. Mere function-granular OVERSHOOT (a long internal-goto loop spanning
several frames in one dispatch) still returns, so it does NOT trip the timeout.

So: if every step returns through the target frame, the recomp does not infinitely
hang in the intro. If a step times out, that frame is where it spins.

Usage:
    python oracle/check_hang.py [--until 4000] [--timeout 15]
"""
from __future__ import annotations
import argparse, json, os, pathlib, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJ = ROOT.parent / "MinishCapRecomp"
RECOMP_EXE = PROJ / "build" / "MinishCapRecomp.exe"

START_KEYINPUT = 0x3F7   # active-low, START pressed
NONE_KEYINPUT = 0x3FF


def held_this_frame(f: int) -> bool:        # identical to diff_intro.py
    return f >= 200 and (f // 6) % 6 == 0


class JsonClient:
    def __init__(self, host, port):
        deadline = time.time() + 10.0
        self.sock = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection((host, port), timeout=2.0)
                break
            except OSError:
                time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError(f"can't reach {host}:{port}")
        self.buf = b""

    def call(self, timeout=None, **kw):
        self.sock.sendall(json.dumps(kw).encode() + b"\n")
        self.sock.settimeout(timeout)
        try:
            while b"\n" not in self.buf:
                ch = self.sock.recv(65536)
                if not ch:
                    raise RuntimeError("peer closed")
                self.buf += ch
        finally:
            self.sock.settimeout(None)
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--until", type=int, default=4000,
                    help="target frame to survive to (intro hang was ~3645)")
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--no-spawn", action="store_true")
    args = ap.parse_args()

    proc = None
    try:
        if not args.no_spawn:
            if not RECOMP_EXE.exists():
                print(f"missing {RECOMP_EXE}", file=sys.stderr); return 1
            # insn-trace OFF for speed (this is a liveness check, not a diff).
            env = dict(os.environ); env.pop("GBARECOMP_INSN_TRACE", None)
            proc = subprocess.Popen([str(RECOMP_EXE), "--tcp", "19842"],
                                    cwd=str(PROJ), env=env,
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
        rec = JsonClient("127.0.0.1", 19842)
        print("==> recomp connected (fresh boot); driving intro to "
              f"f{args.until}", flush=True)
        t0 = time.time()
        for f in range(1, args.until + 1):
            mask = START_KEYINPUT if held_this_frame(f) else NONE_KEYINPUT
            rec.call(cmd="set_keyinput", value=mask)
            try:
                rec.call(timeout=args.timeout, cmd="step")
            except socket.timeout:
                fr = "?"
                print(f"\n*** HANG: step at loop-frame {f} did not return within "
                      f"{args.timeout}s — recomp is spinning in one dispatch. "
                      f"MC-HP-002 NOT resolved. ***", flush=True)
                return 2
            if f % 250 == 0:
                try:
                    info = rec.call(timeout=5.0, cmd="frame")
                    fr = info.get("frame", "?")
                except Exception:
                    fr = "?"
                print(f"  ...loop-frame {f} ok (recomp frame={fr}, "
                      f"{time.time()-t0:.0f}s)", flush=True)
        print(f"\n==> SURVIVED to loop-frame {args.until} with no infinite hang "
              f"({time.time()-t0:.0f}s). The intro spin does not reproduce.",
              flush=True)
        try: rec.call(cmd="quit")
        except Exception: pass
        return 0
    finally:
        if proc:
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill()


if __name__ == "__main__":
    sys.exit(main())
