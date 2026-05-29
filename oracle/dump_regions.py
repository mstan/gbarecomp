#!/usr/bin/env python3
"""dump_regions.py — step recomp + interpreter to a target frame
(phase-aligned at VBlank-start) and dump named IWRAM/EWRAM regions side by
side with per-byte diff marks. Used to read the actual struct contents at a
divergence located by diff_iwram, so the root counter / CPU-written field
can be identified before arming a watchpoint.

Usage:
    python oracle/dump_regions.py --at 40 --hold up \
        --region 0x030043c0:0x40 --region 0x03004460:0x80 \
        --region 0x03004560:0x40
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
    def rd(self, base, n):
        cmd = "read_ewram" if (base >> 24) == 0x02 else "read_iwram"
        r = self.call(cmd=cmd, addr=base, len=n)
        return bytes.fromhex(r["data"]) if r.get("ok") else b""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--at", type=int, default=40)
    ap.add_argument("--hold", default="up")
    ap.add_argument("--region", action="append", default=[],
                    help="base:len, both hex e.g. 0x03004460:0x80")
    args = ap.parse_args()
    mask = KEYMASK.get(args.hold.lower(), 0x3FF)
    regions = []
    for r in args.region:
        b, _, n = r.partition(":")
        regions.append((int(b, 0), int(n, 0)))

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
        for _ in range(args.at):
            rec.call(timeout=20, cmd="step")
            intp.call(timeout=20, cmd="step")
        print(f"=== at frame {args.at} (hold {args.hold}) ===", flush=True)
        for base, n in regions:
            rb, ib = rec.rd(base, n), intp.rd(base, n)
            diffs = [i for i in range(min(len(rb), len(ib))) if rb[i] != ib[i]]
            print(f"\n--- {base:#010x}..{base+n:#010x}  "
                  f"({len(diffs)} diff bytes) ---", flush=True)
            for o in range(0, min(len(rb), len(ib)), 16):
                rr, ii = rb[o:o+16], ib[o:o+16]
                mark = "".join("^^" if rr[k] != ii[k] else "  "
                               for k in range(len(rr)))
                print(f"  {base+o:#010x} R {rr.hex()}", flush=True)
                print(f"             I {ii.hex()}", flush=True)
                if rr != ii:
                    print(f"               {mark}", flush=True)
            for i in diffs:
                print(f"    [{base+i:#010x}] R={rb[i]:#04x} I={ib[i]:#04x} "
                      f"(off +0x{i:x})", flush=True)
        for cl in (rec, intp):
            try: cl.call(cmd="quit")
            except Exception: pass
    finally:
        for p in procs:
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill()


if __name__ == "__main__":
    main()
