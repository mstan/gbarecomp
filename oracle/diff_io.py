#!/usr/bin/env python3
"""diff_io.py — compare interrupt-relevant IO registers recomp vs interp,
frame-by-frame, across a transition. Built for MC-HP-002: the recomp
over-delivers IRQs starting at frame 40 (see diff_counters.py); this checks
whether a NEW interrupt source goes live at f40 with diverging config
(IE/DISPSTAT/timer reload/DMA-enable), which would name the runaway source.

Persistent regs (IE, IME, DISPSTAT, timer controls, DMA enables) survive to the
VBlank-start park; IF is write-1-cleared by then so it usually reads 0 here (the
storm is mid-frame) — IE + device config is the tell.

Usage:
    python oracle/diff_io.py [--from 36 --to 44 --hold up]
"""
from __future__ import annotations
import argparse, json, pathlib, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
import recomp_paths as _rp
PROJ = _rp.game_dir(ROOT)
RECOMP_EXE = _rp.recomp_exe(ROOT)
INTERP_EXE = ROOT / "build" / "bios_smoke.exe"
BIOS = ROOT / "bios" / "gba_bios.bin"
ROM = PROJ / "roms" / "minishcap_usa.gba"
DEF_STATE = PROJ / "roms" / "minishcap_usa.state3"

KEYMASK = {"up": 0x3FF & ~0x40, "down": 0x3FF & ~0x80,
           "left": 0x3FF & ~0x20, "right": 0x3FF & ~0x10, "none": 0x3FF}

# (label, addr, width) — interrupt-relevant IO regs
REGS = [
    ("DISPSTAT", 0x04000004, 2), ("IE", 0x04000200, 2), ("IF", 0x04000202, 2),
    ("IME", 0x04000208, 2),
    ("TM0CNT", 0x04000100, 2), ("TM0CTL", 0x04000102, 2),
    ("TM1CNT", 0x04000104, 2), ("TM1CTL", 0x04000106, 2),
    ("TM2CTL", 0x0400010A, 2), ("TM3CTL", 0x0400010E, 2),
    ("DMA1CNT", 0x040000C6, 2), ("DMA2CNT", 0x040000D2, 2),
    ("DMA3CNT", 0x040000DE, 2),
]


class JsonClient:
    def __init__(self, host, port):
        dl = time.time() + 10.0; self.sock = None; last = None
        while time.time() < dl:
            try:
                self.sock = socket.create_connection((host, port), timeout=2.0); break
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
                if not ch: raise RuntimeError("peer closed")
                self.buf += ch
        finally:
            self.sock.settimeout(None)
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode())

    def io(self, addr, width):
        r = self.call(cmd="read_io", addr=addr, len=width)
        if not r.get("ok"): return None
        b = bytes.fromhex(r["data"])
        return int.from_bytes(b, "little")

    def snap(self):
        return {lbl: self.io(a, w) for lbl, a, w in REGS}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default=str(DEF_STATE))
    ap.add_argument("--from", dest="f0", type=int, default=36)
    ap.add_argument("--to", dest="f1", type=int, default=44)
    ap.add_argument("--hold", default="up")
    ap.add_argument("--timeout", type=float, default=12.0)
    ap.add_argument("--no-spawn", action="store_true")
    args = ap.parse_args()
    mask = KEYMASK.get(args.hold.lower(), 0x3FF)

    procs = []
    try:
        if not args.no_spawn:
            procs.append(subprocess.Popen(
                [str(RECOMP_EXE), "--tcp", "19842"], cwd=str(PROJ),
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            procs.append(subprocess.Popen(
                [str(INTERP_EXE), "--bios", str(BIOS), "--rom", str(ROM),
                 "--tcp", "19844"], cwd=str(ROOT),
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        rec = JsonClient("127.0.0.1", 19842)
        intp = JsonClient("127.0.0.1", 19844)
        for cl in (rec, intp):
            if not cl.call(cmd="savestate_load", path=args.state).get("ok"):
                print("load failed"); return 1
            cl.call(cmd="set_keyinput", value=mask)
        print("==> loaded, holding", args.hold, flush=True)

        rec_dead = intp_dead = False
        for f in range(1, args.f1 + 1):
            if f >= args.f0 and not rec_dead and not intp_dead:
                rs, is_ = rec.snap(), intp.snap()
                diffs = [f"{lbl}:R={rs[lbl]:#06x}/I={is_[lbl]:#06x}"
                         for lbl, _, _ in REGS
                         if rs[lbl] is not None and is_[lbl] is not None
                         and rs[lbl] != is_[lbl]]
                same = [f"{lbl}={rs[lbl]:#x}" for lbl, _, _ in REGS
                        if rs[lbl] == is_[lbl] and rs[lbl] is not None]
                tag = ("  DIFF: " + " ".join(diffs)) if diffs else "  (all equal)"
                print(f"f{f:3d}{tag}", flush=True)
                if diffs:
                    print("       same: " + " ".join(same), flush=True)
            if not rec_dead:
                try: rec.call(timeout=args.timeout, cmd="step")
                except socket.timeout: print(f"  recomp SPUN f{f}"); rec_dead = True
            if not intp_dead:
                try: intp.call(timeout=args.timeout, cmd="step")
                except socket.timeout: print(f"  interp SPUN f{f}"); intp_dead = True
            if rec_dead: break
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
