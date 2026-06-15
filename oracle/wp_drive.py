#!/usr/bin/env python3
"""wp_drive.py — drive the recomp with the diff_intro START pulse to a target
frame while a memory-write watchpoint is armed (env GBARECOMP_ABORT_ON_MEM_WRITE_*),
capturing the recomp's stderr trace dump to a file. Used to pin the writer +
control flow of a diverging byte (MC-HP-002: 0x03001753, the M4A track +0x1b
countdown that decrements one frame early at f610).

Unlike diff_cycle_trace.py, this routes the recomp's stderr to --stderr so the
abort dump (runtime_trace_dump_recent) is visible.

Usage:
  GBARECOMP_RECOMP_EXE=.../MinishCapRecomp.exe \
  GBARECOMP_ABORT_ON_MEM_WRITE_ADDR=0x03001753 \
  GBARECOMP_ABORT_ON_MEM_WRITE_MIN_FRAME=610 \
  GBARECOMP_ABORT_ON_MEM_WRITE_VALUE=0x4d \
  GBARECOMP_TRACE_DUMP_DEPTH=400 \
  python oracle/wp_drive.py --until 620 --stderr /tmp/wp.log
"""
from __future__ import annotations
import argparse, json, os, pathlib, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJ = ROOT.parent / "MinishCapRecomp"
RECOMP_EXE = pathlib.Path(os.environ.get(
    "GBARECOMP_RECOMP_EXE", PROJ / "build" / "MinishCapRecomp.exe"))
START_KEYINPUT = 0x3F7
NONE_KEYINPUT = 0x3FF


FRAME_CYC = 280896


def held(f):  # identical schedule to diff_cycle_trace.py
    return f >= 200 and (f // 6) % 6 == 0


def held_cycle(c):
    f = c // FRAME_CYC
    return f >= 200 and (f // 6) % 6 == 0


class C:
    def __init__(self, port):
        dl = time.time() + 10
        self.s = None
        while time.time() < dl:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=2); break
            except OSError:
                time.sleep(0.1)
        if not self.s:
            raise RuntimeError("no connect")
        self.b = b""

    def call(self, timeout=None, **kw):
        self.s.sendall(json.dumps(kw).encode() + b"\n")
        self.s.settimeout(timeout)
        try:
            while b"\n" not in self.b:
                ch = self.s.recv(65536)
                if not ch:
                    raise RuntimeError("peer closed")
                self.b += ch
        finally:
            self.s.settimeout(None)
        line, _, self.b = self.b.partition(b"\n")
        return json.loads(line.decode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--until", type=int, default=620)
    ap.add_argument("--stderr", default="/tmp/wp.log")
    ap.add_argument("--timeout", type=float, default=90)
    ap.add_argument("--input", choices=("pulse", "cycle"), default="pulse")
    args = ap.parse_args()

    errf = open(args.stderr, "wb")
    p = subprocess.Popen([str(RECOMP_EXE), "--tcp", "19842"], cwd=str(PROJ),
                         env=dict(os.environ),
                         stdout=subprocess.DEVNULL, stderr=errf)
    try:
        rec = C(19842)
        print(f"==> driving START pulse to f{args.until} (watchpoint armed)", flush=True)
        # Drive a FIXED number of step calls (not "until reported frame"): the
        # recomp's frame counter is offset ~+79 from driver frames by the boot
        # step_frame overshoot, so a reported-frame loop stops ~79 frames early
        # and never reaches the spin. NB: the watchpoint MIN_FRAME gate is in
        # g_runtime_vblank_starts units (~spin+79), not driver frames.
        fired = False
        for f in range(1, args.until + 1):
            if args.input == "cycle":
                c = rec.call(cmd="counters").get("cycles_elapsed", 0)
                mask = START_KEYINPUT if held_cycle(c) else NONE_KEYINPUT
            else:
                mask = START_KEYINPUT if held(f) else NONE_KEYINPUT
            rec.call(cmd="set_keyinput", value=mask)
            try:
                rec.call(timeout=args.timeout, cmd="step")
            except Exception as e:
                print(f"==> recomp died at driver-f{f} (watchpoint hit / spin?): {e}",
                      flush=True)
                fired = True
                break
        if not fired:
            print(f"==> drove {args.until} frames without watchpoint firing", flush=True)
            try: rec.call(cmd="quit")
            except Exception: pass
    finally:
        try: p.wait(timeout=8)
        except subprocess.TimeoutExpired: p.kill()
        errf.close()
    print(f"\n===== recomp stderr ({args.stderr}) =====", flush=True)
    sys.stdout.write(pathlib.Path(args.stderr).read_text(errors="replace"))


if __name__ == "__main__":
    sys.exit(main())
