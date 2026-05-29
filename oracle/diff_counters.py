#!/usr/bin/env python3
"""diff_counters.py — MC-HP-002 root: is the recomp a game-frame AHEAD because
it delivers an extra/early VBlank IRQ (or runs more cycles per frame) than the
interpreter oracle?

The instruction audit (no m4a miscompile) and the wake-from-HALT IRQ-phase fix
(kGbaIrqDelayCycles — byte-identical repro) both came up empty, leaving ONE
surviving hypothesis: the recomp advances ~1 game-logic frame ahead through the
transition. Game logic steps once per VBlankIntrWait release = once per VBlank
IRQ, so if the recomp delivers an EXTRA or EARLY VBlank IRQ (PPU double-firing
`vblank_started`, or an IF bit not cleared and re-firing), or simply fits one
more logic frame per PPU frame (cycle-accounting drift), it gets ahead.

This compares the phase-robust cumulative hardware-event counters (the TCP
`counters` command: irq_entries, vblank_irqs_raised, swi_entries,
cycles_elapsed) frame-by-frame on recomp (19842) vs interpreter (19844) from the
same state3, holding the same key. Counters are cumulative hardware-event tallies
— robust to the post/pre-handler park-phase skew that contaminates raw memory
diffs. The frame where a per-frame DELTA first diverges localizes the gain.

Usage:
    python oracle/diff_counters.py [--state <path>] [--frames 50] [--hold up]
"""
from __future__ import annotations
import argparse, json, pathlib, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent          # gbarecomp/
PROJ = ROOT.parent / "MinishCapRecomp"
RECOMP_EXE = PROJ / "build" / "MinishCapRecomp.exe"
INTERP_EXE = ROOT / "build" / "bios_smoke.exe"
BIOS = ROOT / "bios" / "gba_bios.bin"
ROM = PROJ / "roms" / "minishcap_usa.gba"
DEF_STATE = PROJ / "roms" / "minishcap_usa.state3"

KEYMASK = {"up": 0x3FF & ~0x40, "down": 0x3FF & ~0x80,
           "left": 0x3FF & ~0x20, "right": 0x3FF & ~0x10, "none": 0x3FF}

FIELDS = ["irq_entries", "vblank_irqs_raised", "swi_entries",
          "halt_steps", "cycles_elapsed"]


class JsonClient:
    def __init__(self, host, port):
        deadline = time.time() + 10.0
        self.sock = None
        last = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection((host, port), timeout=2.0)
                break
            except OSError as e:
                last = e; time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError(f"can't reach {host}:{port}: {last}")
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

    def counters(self):
        r = self.call(cmd="counters")
        if not r.get("ok"):
            raise RuntimeError(f"counters: {r}")
        return {k: r.get(k, 0) for k in FIELDS}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default=str(DEF_STATE))
    ap.add_argument("--frames", type=int, default=50)
    ap.add_argument("--hold", default="up")
    ap.add_argument("--timeout", type=float, default=12.0)
    ap.add_argument("--no-spawn", action="store_true")
    args = ap.parse_args()
    mask = KEYMASK.get(args.hold.lower(), 0x3FF)

    procs = []
    try:
        if not args.no_spawn:
            for exe in (RECOMP_EXE, INTERP_EXE):
                if not exe.exists():
                    print(f"missing {exe}", file=sys.stderr); return 1
            procs.append(subprocess.Popen(
                [str(RECOMP_EXE), "--tcp", "19842"], cwd=str(PROJ),
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            procs.append(subprocess.Popen(
                [str(INTERP_EXE), "--bios", str(BIOS), "--rom", str(ROM),
                 "--tcp", "19844"], cwd=str(ROOT),
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

        rec = JsonClient("127.0.0.1", 19842)
        intp = JsonClient("127.0.0.1", 19844)
        print("==> connected: recomp=19842 interp=19844", flush=True)

        for who, cl in (("recomp", rec), ("interp", intp)):
            r = cl.call(cmd="savestate_load", path=args.state)
            if not r.get("ok"):
                print(f"{who} savestate_load failed: {r}"); return 1
            cl.call(cmd="set_keyinput", value=mask)
        print("==> both loaded, holding", args.hold, flush=True)

        prev = {"recomp": rec.counters(), "interp": intp.counters()}
        print("    (per-frame deltas; R=recomp I=interp; * = delta mismatch)")
        hdr = "f   " + "  ".join(f"{k.split('_')[0][:5]:>11}" for k in FIELDS)
        print(hdr, flush=True)

        rec_dead = intp_dead = False
        for f in range(1, args.frames + 1):
            # step both first, then read cumulative counters → per-frame delta
            if not rec_dead:
                try: rec.call(timeout=args.timeout, cmd="step")
                except socket.timeout:
                    print(f"   recomp SPUN at frame {f}", flush=True); rec_dead = True
            if not intp_dead:
                try: intp.call(timeout=args.timeout, cmd="step")
                except socket.timeout:
                    print(f"   interp SPUN at frame {f}", flush=True); intp_dead = True

            rc = rec.counters() if not rec_dead else None
            ic = intp.counters() if not intp_dead else None
            cells = []
            for k in FIELDS:
                rd = (rc[k] - prev["recomp"][k]) if rc else None
                idv = (ic[k] - prev["interp"][k]) if ic else None
                star = "*" if (rd is not None and idv is not None and rd != idv) else " "
                rs = "--" if rd is None else str(rd)
                is_ = "--" if idv is None else str(idv)
                cells.append(f"{star}R{rs:>3}/I{is_:>3}")
            print(f"f{f:3d} " + "  ".join(cells), flush=True)
            if rc: prev["recomp"] = rc
            if ic: prev["interp"] = ic
            if rec_dead and intp_dead:
                break
        for cl in (rec, intp):
            try: cl.call(cmd="quit")
            except Exception: pass
        return 0
    finally:
        for p in procs:
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill()


if __name__ == "__main__":
    sys.exit(main())
