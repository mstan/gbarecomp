#!/usr/bin/env python3
"""irq_sources.py — pin the MC-HP-002 IRQ-storm source. Steps the recomp to a
frame just before the spin, then dumps the always-on runtime trace ring and
histograms the IRQ events by source. runtime_irq records (IE & IF) in the trace
addr field, so each IRQ event names which interrupt was being vectored.

Usage:
    python oracle/irq_sources.py [--to 46 --hold up]
"""
from __future__ import annotations
import argparse, json, pathlib, socket, subprocess, sys, time
from collections import Counter

ROOT = pathlib.Path(__file__).resolve().parent.parent
import recomp_paths as _rp
PROJ = _rp.game_dir(ROOT)
RECOMP_EXE = _rp.recomp_exe(ROOT)
DEF_STATE = PROJ / "roms" / "minishcap_usa.state3"

KEYMASK = {"up": 0x3FF & ~0x40, "down": 0x3FF & ~0x80,
           "left": 0x3FF & ~0x20, "right": 0x3FF & ~0x10, "none": 0x3FF}
SRC = {0: "VBlank", 1: "HBlank", 2: "VCount", 3: "TM0", 4: "TM1", 5: "TM2",
       6: "TM3", 7: "Serial", 8: "DMA0", 9: "DMA1", 10: "DMA2", 11: "DMA3",
       12: "Key", 13: "GPak"}


def srcs(v):
    return "+".join(SRC.get(b, str(b)) for b in range(16) if v & (1 << b)) or f"0x{v:x}"


class C:
    def __init__(self, port):
        dl = time.time() + 10; self.s = None
        while time.time() < dl:
            try: self.s = socket.create_connection(("127.0.0.1", port), timeout=2); break
            except OSError: time.sleep(0.1)
        if not self.s: raise RuntimeError("no connect")
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default=str(DEF_STATE))
    ap.add_argument("--to", type=int, default=46)
    ap.add_argument("--hold", default="up")
    ap.add_argument("--port", type=int, default=19842)
    args = ap.parse_args()
    mask = KEYMASK.get(args.hold.lower(), 0x3FF)

    p = subprocess.Popen([str(RECOMP_EXE), "--tcp", str(args.port)], cwd=str(PROJ),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        cl = C(args.port)
        print("load:", cl.call(cmd="savestate_load", path=args.state).get("ok"))
        cl.call(cmd="set_keyinput", value=mask)
        spun = None
        for f in range(1, args.to + 1):
            try: cl.call(timeout=12, cmd="step")
            except socket.timeout:
                spun = f; print(f"SPUN at f{f}"); break
        r = cl.call(cmd="runtime_trace", max=4096)
        ents = r.get("entries", [])
        irqs = [e for e in ents if e["kind"] == 6]
        print(f"ring window={len(ents)} irq_events={len(irqs)} (stepped to f{spun or args.to})")
        c = Counter(srcs(e["addr"]) for e in irqs)
        print("IRQ source histogram (last <=512 ring slots):")
        for k, v in c.most_common():
            print(f"   {v:4d}  {k}")
        print("last 20 IRQ events (depth=aux):")
        for e in irqs[-20:]:
            print(f"   seq={e['seq']} src={srcs(e['addr'])} ret=0x{e['pc']:08x} "
                  f"mode=0x{e['cpsr'] & 0x1f:02x} depth={e.get('aux', '?')}")
        try: cl.call(cmd="quit")
        except Exception: pass
        return 0
    finally:
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()


if __name__ == "__main__":
    sys.exit(main())
