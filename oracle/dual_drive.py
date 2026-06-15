#!/usr/bin/env python3
"""dual_drive.py — boot BOTH the recomp and the interp oracle fresh, drive the
identical diff_intro START pulse to --until frames, and quit (so each engine
dumps its always-on event logs). This is a DRIVER ONLY — it does no per-frame
lockstep diffing (that mis-fires on phase offsets); the analysis is done offline
on the dumped logs (GBARECOMP_IRQ_LOG / GBARECOMP_SWI_LOG → CSV + .interp.csv),
which carry cycle stamps so the recomp and oracle sequences align by hardware
time, per recomp-template "order + state + caller, not absolute frames".

Set the log env vars before running, e.g.:
  GBARECOMP_SWI_LOG=/tmp/swi.csv GBARECOMP_RECOMP_EXE=.../build-selfheal/MinishCapRecomp.exe \
  python oracle/dual_drive.py --until 290
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


FRAME_CYC = 280896  # GbaPpu::kCyclesPerFrame


def held(f):
    return f >= 200 and (f // 6) % 6 == 0


def held_cycle(c):
    # held() re-expressed in cumulative-CYCLE space so both engines get the
    # identical START schedule at the identical cycle, immune to the boot
    # frame-counting skew (recomp step_frame overshoots ~9 frames at boot, so
    # frame-indexed input desyncs the two engines at equal cycle). f -> c/FRAME_CYC.
    f = c // FRAME_CYC
    return f >= 200 and (f // 6) % 6 == 0


def input_mask(f, mode, cyc=0):
    if mode == "hold":
        return START_KEYINPUT
    if mode == "none":
        return NONE_KEYINPUT
    if mode == "cycle":
        return START_KEYINPUT if held_cycle(cyc) else NONE_KEYINPUT
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
    ap.add_argument("--until", type=int, default=290)
    ap.add_argument("--timeout", type=float, default=90)
    ap.add_argument("--detect-spin", action="store_true",
                    help="on a step timeout, mark that engine spun and keep "
                         "driving the other (to see if BOTH spin)")
    ap.add_argument("--input", choices=("pulse", "hold", "none", "cycle"),
                    default="pulse",
                    help="input schedule: pulse=frame-indexed START spam (jitters "
                         "under boot frame skew); cycle=same pattern but keyed on "
                         "each engine's cumulative cycle (identical input at equal "
                         "cycle); hold/none=constant input")
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
        print(f"==> driving both to f{args.until} (START pulse); "
              f"--detect-spin={args.detect_spin}", flush=True)
        rec_spun = intp_spun = None

        def cyc_of(cl):
            try:
                return cl.call(cmd="counters").get("cycles_elapsed", 0)
            except Exception:
                return 0

        for f in range(1, args.until + 1):
            if rec_spun is None:
                m = input_mask(f, args.input,
                               cyc_of(rec) if args.input == "cycle" else 0)
                rec.call(cmd="set_keyinput", value=m)
                try:
                    rec.call(timeout=args.timeout, cmd="step")
                except (socket.timeout, OSError):
                    rec_spun = f
                    print(f"!! RECOMP spun (step timeout) at frame {f}", flush=True)
                    if not args.detect_spin: raise
            if intp_spun is None:
                m = input_mask(f, args.input,
                               cyc_of(intp) if args.input == "cycle" else 0)
                intp.call(cmd="set_keyinput", value=m)
                try:
                    intp.call(timeout=args.timeout, cmd="step")
                except (socket.timeout, OSError):
                    intp_spun = f
                    print(f"!! INTERP spun (step timeout) at frame {f}", flush=True)
            if f % 500 == 0:
                print(f"  ...frame {f} (rec_spun={rec_spun} intp_spun={intp_spun})",
                      flush=True)
            if rec_spun and intp_spun:
                break
        print(f"\n==> RESULT: recomp spun at {rec_spun}, interp spun at {intp_spun}",
              flush=True)
        for cl in (rec, intp):
            try: cl.call(cmd="quit")
            except Exception: pass
    finally:
        for p in procs:
            try: p.wait(timeout=8)
            except subprocess.TimeoutExpired: p.kill()


if __name__ == "__main__":
    sys.exit(main())
