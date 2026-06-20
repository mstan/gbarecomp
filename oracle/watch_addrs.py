#!/usr/bin/env python3
"""watch_addrs.py — dump a fixed set of addresses on recomp vs interpreter
each frame (phase-aligned at VBlank-start) to characterize the baseline
divergence found by diff_iwram (gMain+0, IRQ stack, IntrWait flag).
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
import recomp_paths as _rp
PROJ = _rp.game_dir(ROOT)
RECOMP_EXE = _rp.recomp_exe(ROOT)
INTERP_EXE = ROOT / "build" / "bios_smoke.exe"
BIOS = ROOT / "bios" / "gba_bios.bin"
ROM = PROJ / "roms" / "minishcap_usa.gba"
STATE = PROJ / "roms" / "minishcap_usa.state3"
UP = 0x3FF & ~0x40

WATCH = [
    (0x03001000, 16, "gMain"),
    (0x030043d0, 16, "0x030043d0"),
    (0x03007ec0, 24, "IRQstack@7ec0"),
    (0x03007fe0, 32, "IRQarea@7fe0 (IntrWait flag @+0x18=7ff8)"),
]
FRAMES = int(sys.argv[1]) if len(sys.argv) > 1 else 3


class Cl:
    def __init__(self, port):
        dl = time.time() + 10; self.s = None
        while time.time() < dl:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=2); break
            except OSError: time.sleep(0.1)
        if not self.s: raise RuntimeError(f"no connect :{port}")
        self.b = b""
    def call(self, timeout=None, **kw):
        self.s.sendall(json.dumps(kw).encode() + b"\n"); self.s.settimeout(timeout)
        try:
            while b"\n" not in self.b:
                ch = self.s.recv(1 << 20)
                if not ch: raise RuntimeError("closed")
                self.b += ch
        finally: self.s.settimeout(None)
        line, _, self.b = self.b.partition(b"\n"); return json.loads(line.decode())
    def rd(self, base, n):
        r = self.call(cmd="read_iwram", addr=base, len=n)
        return bytes.fromhex(r["data"]) if r.get("ok") else b""


def main():
    procs = [
        subprocess.Popen([str(RECOMP_EXE), "--tcp", "19842"], cwd=str(PROJ),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
        subprocess.Popen([str(INTERP_EXE), "--bios", str(BIOS), "--rom", str(ROM),
                          "--tcp", "19844"], cwd=str(ROOT),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
    try:
        rec, intp = Cl(19842), Cl(19844)
        for cl in (rec, intp):
            cl.call(cmd="savestate_load", path=str(STATE))
            cl.call(cmd="set_keyinput", value=UP)
        for f in range(FRAMES + 1):
            print(f"=== frame {f} ===", flush=True)
            for base, n, name in WATCH:
                rb = rec.rd(base, n); ib = intp.rd(base, n)
                mark = "  DIFF" if rb != ib else ""
                print(f"  {name}:{mark}", flush=True)
                print(f"    R {rb.hex()}", flush=True)
                print(f"    I {ib.hex()}", flush=True)
            try: rec.call(timeout=15, cmd="step")
            except socket.timeout: print("  recomp SPUN"); break
            intp.call(timeout=15, cmd="step")
        for cl in (rec, intp):
            try: cl.call(cmd="quit")
            except Exception: pass
    finally:
        for p in procs:
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill()


if __name__ == "__main__":
    main()
