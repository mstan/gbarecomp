#!/usr/bin/env python3
"""track_bytes.py — print the value of one or more bytes on recomp vs
interpreter each frame across a range. Used to tell a constant phase offset
(recomp parks one VBlank-handler ahead of interp) from a real accumulating
divergence (recomp ticks an engine more often than interp).

Usage:
    python oracle/track_bytes.py --from 34 --to 50 --hold up \
        --addr 0x03004470 --addr 0x030043d4
"""
from __future__ import annotations
import argparse, json, pathlib, socket, subprocess, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJ = ROOT.parent / "MinishCapRecomp"
RECOMP_EXE = PROJ / "build" / "MinishCapRecomp.exe"
INTERP_EXE = ROOT / "build" / "bios_smoke.exe"
BIOS = ROOT / "bios" / "gba_bios.bin"
ROM = PROJ / "roms" / "minishcap_usa.gba"
STATE = PROJ / "roms" / "minishcap_usa.state3"
KEYMASK = {"up": 0x3FF & ~0x40, "down": 0x3FF & ~0x80, "left": 0x3FF & ~0x20,
           "right": 0x3FF & ~0x10, "a": 0x3FF & ~0x01, "none": 0x3FF}


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
    def b1(self, addr):
        cmd = "read_ewram" if (addr >> 24) == 0x02 else "read_iwram"
        r = self.call(cmd=cmd, addr=addr, len=1)
        return r["data"] if r.get("ok") else "??"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="lo", type=int, default=34)
    ap.add_argument("--to", dest="hi", type=int, default=50)
    ap.add_argument("--hold", default="up")
    ap.add_argument("--addr", action="append", default=[])
    args = ap.parse_args()
    mask = KEYMASK.get(args.hold.lower(), 0x3FF)
    addrs = [int(a, 0) for a in args.addr]

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
            cl.call(cmd="set_keyinput", value=mask)
        hdr = "frame " + " ".join(f"{a:#010x}[R/I/d]" for a in addrs)
        print(hdr, flush=True)
        rec_dead = False
        for f in range(0, args.hi + 1):
            if f >= args.lo:
                cells = []
                for a in addrs:
                    rv = "--" if rec_dead else rec.b1(a)
                    iv = intp.b1(a)
                    try: d = int(rv, 16) - int(iv, 16)
                    except Exception: d = "?"
                    cells.append(f"R={rv} I={iv} d={d}")
                print(f"f{f:3d}  " + "   ".join(cells), flush=True)
            if not rec_dead:
                try: rec.call(timeout=15, cmd="step")
                except socket.timeout:
                    print(f"   recomp SPUN at f{f}", flush=True); rec_dead = True
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
