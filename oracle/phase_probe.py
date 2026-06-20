#!/usr/bin/env python3
"""phase_probe.py — confirm whether the recomp and interpreter `step`
commands park at the SAME PPU phase.

Hypothesis (from reading the harness): the interpreter `step`
(step_one_frame) stops at VBlank-start (scanline 159->160), explicitly
to match mGBA's runFrame; the recomp `step` (step_frame) stops when
ppu.frame_count() changes, which happens at scanline-WRAP (227->0).
Those are 68 scanlines apart. If true, diff_anim has been comparing two
different mid-frame moments and its "recomp runs 1 frame ahead" finding
is partly a harness artifact.

This probe loads the same savestate into both, holds the same input, and
after each `step` prints VCOUNT / DISPSTAT / frame from each side. If the
recomp consistently parks near VCOUNT 0 while the interpreter parks near
VCOUNT 160, the offset is confirmed.
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


class C:
    def __init__(self, port):
        dl = time.time() + 10
        self.s = None
        while time.time() < dl:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
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
                    raise RuntimeError("closed")
                self.b += ch
        finally:
            self.s.settimeout(None)
        line, _, self.b = self.b.partition(b"\n")
        return json.loads(line.decode())

    def phase(self):
        r = self.call(cmd="ppu_state")
        return r.get("vcount"), r.get("dispstat"), r.get("frame")


def main():
    procs = [
        subprocess.Popen([str(RECOMP_EXE), "--tcp", "19842"], cwd=str(PROJ),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
        subprocess.Popen([str(INTERP_EXE), "--bios", str(BIOS), "--rom",
                          str(ROM), "--tcp", "19844"], cwd=str(ROOT),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
    ]
    try:
        rec, intp = C(19842), C(19844)
        for who, cl in (("recomp", rec), ("interp", intp)):
            r = cl.call(cmd="savestate_load", path=str(STATE))
            if not r.get("ok"):
                print(f"{who} load failed: {r}"); return 1
            cl.call(cmd="set_keyinput", value=UP)
        def line(tag):
            rv, rd, rf = rec.phase()
            iv, idsp, ifr = intp.phase()
            print(f"{tag:>8}: recomp[vcount={rv:>3} dispstat={rd:#06x} frame={rf}]"
                  f"  interp[vcount={iv:>3} dispstat={idsp:#06x} frame={ifr}]",
                  flush=True)
        line("@load")
        for i in range(1, 9):
            for cl in (rec, intp):
                try:
                    cl.call(timeout=12, cmd="step")
                except socket.timeout:
                    print(f"   step {i} timed out (spin)");
            line(f"step{i}")
        for cl in (rec, intp):
            try: cl.call(cmd="quit")
            except Exception: pass
    finally:
        for p in procs:
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
