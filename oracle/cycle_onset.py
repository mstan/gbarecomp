#!/usr/bin/env python3
"""cycle_onset.py — find the TRUE first divergence FRAME between the recomp and
the save-matched interp oracle, robustly, at frame granularity.

Both engines receive the IDENTICAL per-frame input mask, and (verified) execute
cycle-locked — identical cumulative `cycles_elapsed` — for as long as they run
the same instruction stream. So the first frame where any hardware counter
(cycles_elapsed, irq_entries, swi_entries, vblank_irqs_raised) differs is the
onset of the real execution divergence. This sidesteps the cycle-ANCHOR walk in
fp_cycle_diff.py, which re-syncs on a coincidental later (cycle,pc,regs) match
once drift starts and then reports a downstream KEYINPUT-read phantom.

Once this prints the onset frame F, run:
  fp_cycle_diff.py --frame F --input <same> --ctx 30
for a tight cycle-aligned instruction window around the real first divergence.

  GBARECOMP_LOAD_SAV=1 GBARECOMP_RECOMP_EXE=.../build-selfheal/MinishCapRecomp.exe \
  python oracle/cycle_onset.py --until 3700 --input pulse
"""
from __future__ import annotations
import argparse, json, os, pathlib, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJ = ROOT.parent / "MinishCapRecomp"
RECOMP_EXE = pathlib.Path(os.environ.get(
    "GBARECOMP_RECOMP_EXE", PROJ / "build" / "MinishCapRecomp.exe"))
INTERP_EXE = pathlib.Path(os.environ.get(
    "GBARECOMP_INTERP_EXE", ROOT / "build" / "bios_smoke.exe"))
BIOS = ROOT / "bios" / "gba_bios.bin"
ROM = PROJ / "roms" / "minishcap_usa.gba"
START_KEYINPUT = 0x3F7
NONE_KEYINPUT = 0x3FF
WATCH = ("cycles_elapsed", "irq_entries", "swi_entries", "vblank_irqs_raised")


def held(f):
    return f >= 200 and (f // 6) % 6 == 0


def input_mask(f, mode):
    if mode == "hold":
        return START_KEYINPUT
    if mode == "none":
        return NONE_KEYINPUT
    return START_KEYINPUT if held(f) else NONE_KEYINPUT


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
            raise RuntimeError(f"no connect :{port}")
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
    ap.add_argument("--until", type=int, default=3700)
    ap.add_argument("--timeout", type=float, default=20)
    ap.add_argument("--input", choices=("pulse", "hold", "none"), default="pulse")
    args = ap.parse_args()
    env = dict(os.environ, GBARECOMP_INSN_TRACE="0")
    procs = [
        subprocess.Popen([str(RECOMP_EXE), "--tcp", "19842"], cwd=str(PROJ), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
        subprocess.Popen([str(INTERP_EXE), "--bios", str(BIOS), "--rom", str(ROM),
                          "--tcp", "19844"], cwd=str(ROOT), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
    ]
    try:
        rec, intp = C(19842), C(19844)
        print(f"==> driving both to f{args.until} (input={args.input}); "
              f"comparing {', '.join(WATCH)} each frame", flush=True)
        prev = None
        for f in range(1, args.until + 1):
            mask = input_mask(f, args.input)
            rec.call(cmd="set_keyinput", value=mask)
            intp.call(cmd="set_keyinput", value=mask)
            try:
                rec.call(timeout=args.timeout, cmd="step")
                intp.call(timeout=args.timeout, cmd="step")
            except (socket.timeout, OSError):
                print(f"!! step timeout at frame {f} (one engine spun); "
                      f"last matched frame {prev}", flush=True)
                break
            rc = rec.call(cmd="counters")
            ic = intp.call(cmd="counters")
            diffs = [(k, rc.get(k), ic.get(k)) for k in WATCH
                     if rc.get(k) != ic.get(k)]
            if diffs:
                print(f"\n*** FIRST COUNTER DIVERGENCE at frame {f} "
                      f"(input mask=0x{mask:03x}, START "
                      f"{'DOWN' if mask == START_KEYINPUT else 'up'}) ***",
                      flush=True)
                for k, a, b in diffs:
                    print(f"   {k}: recomp={a} interp={b} (delta {a-b:+d})")
                print(f"   last fully-matched frame: {prev}", flush=True)
                # dump the full counter sets for context
                print(f"   recomp counters: {rc}")
                print(f"   interp counters: {ic}", flush=True)
                break
            prev = f
            if f % 200 == 0:
                print(f"  ...frame {f} locked (cyc={rc.get('cycles_elapsed')})",
                      flush=True)
        else:
            print(f"==> no counter divergence through f{args.until}", flush=True)
        for cl in (rec, intp):
            try: cl.call(cmd="quit")
            except Exception: pass
    finally:
        for p in procs:
            try: p.wait(timeout=8)
            except subprocess.TimeoutExpired: p.kill()


if __name__ == "__main__":
    sys.exit(main())
