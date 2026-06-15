#!/usr/bin/env python3
"""spin_mem_diff.py — the MC-HP-002 spin is a busy-wait command-list walker at
0x08004286 whose registers are IDENTICAL to the interp on entry (proven by
fp_cycle_diff --input cycle), so the divergence is a WRONG MEMORY value in the
structure the loop walks (the register-only fp ring can't see it).

This breaks the recomp at the spin entry (set_break_pc), captures r0/r1 and the
cumulative cycle, and dumps the memory the loop reads. It then drives the interp
to the same cycle and dumps the same region. A byte diff localizes the corrupt
field; the structure's address region (IWRAM/EWRAM) tells us who should have
written it. Pure observation.

  GBARECOMP_LOAD_SAV=1 GBARECOMP_RECOMP_EXE=.../build-selfheal/MinishCapRecomp.exe \
  python oracle/spin_mem_diff.py
"""
from __future__ import annotations
import argparse, json, os, pathlib, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJ = ROOT.parent / "MinishCapRecomp"
REC = os.environ.get("GBARECOMP_RECOMP_EXE", str(PROJ / "build" / "MinishCapRecomp.exe"))
INT = os.environ.get("GBARECOMP_INTERP_EXE", str(ROOT / "build" / "bios_smoke.exe"))
BIOS = str(ROOT / "bios" / "gba_bios.bin")
ROM = str(PROJ / "roms" / "minishcap_usa.gba")
FRAME_CYC = 280896
SPIN_PC = 0x0800428C


def held_cycle(c):
    f = c // FRAME_CYC
    return f >= 200 and (f // 6) % 6 == 0


class Cl:
    def __init__(self, p):
        d = time.time() + 10; self.s = None
        while time.time() < d:
            try:
                self.s = socket.create_connection(("127.0.0.1", p), timeout=2); break
            except OSError:
                time.sleep(0.1)
        self.b = b""

    def call(self, timeout=None, **k):
        self.s.sendall(json.dumps(k).encode() + b"\n"); self.s.settimeout(timeout)
        try:
            while b"\n" not in self.b:
                ch = self.s.recv(65536)
                if not ch: raise RuntimeError("closed")
                self.b += ch
        finally:
            self.s.settimeout(None)
        l, _, self.b = self.b.partition(b"\n"); return json.loads(l.decode())


def cyc(cl):
    return cl.call(cmd="counters").get("cycles_elapsed", 0)


def step(cl, f):
    cl.call(cmd="set_keyinput", value=0x3F7 if held_cycle(cyc(cl)) else 0x3FF)
    cl.call(timeout=30, cmd="step")


def region_cmd(addr):
    if 0x02000000 <= addr < 0x02040000: return "read_ewram"
    if 0x03000000 <= addr < 0x03008000: return "read_iwram"
    if 0x08000000 <= addr: return None  # ROM — not exposed by a read cmd here
    return None


def readmem(cl, addr, length):
    cmd = region_cmd(addr)
    if cmd is None:
        return None
    r = cl.call(cmd=cmd, addr=addr, len=length)
    return bytes.fromhex(r["data"]) if r.get("ok") else None


def regs(cl):
    r = cl.call(cmd="registers")
    return {f"r{i}": r.get(f"r{i}") for i in range(15)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rec-frame", type=int, default=3651)
    ap.add_argument("--win", type=lambda s: int(s, 0), default=0x80)
    args = ap.parse_args()
    env = dict(os.environ, GBARECOMP_INSN_TRACE="0")
    pr = subprocess.Popen([REC, "--tcp", "19842"], cwd=str(PROJ), env=env,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pi = subprocess.Popen([INT, "--bios", BIOS, "--rom", ROM, "--tcp", "19844"],
                          cwd=str(ROOT), env=env,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        rec = Cl(19842); intp = Cl(19844)
        rec.call(cmd="set_break_pc", value=SPIN_PC)
        print(f"==> driving recomp to f{args.rec_frame} (breaks at spin "
              f"0x{SPIN_PC:08x})", flush=True)
        for f in range(1, args.rec_frame + 1):
            step(rec, f)
        rr = regs(rec); rcyc = cyc(rec)
        print(f"   recomp @spin: cyc={rcyc} r0=0x{rr['r0']:08x} r1=0x{rr['r1']:08x} "
              f"r2=0x{rr['r2']:08x} r3=0x{rr['r3']:08x}", flush=True)
        r0 = rr["r0"]; r1 = rr["r1"]
        # original list pointer = *(r0+0x5c)
        p5c = readmem(rec, r0 + 0x5c, 4)
        orig = int.from_bytes(p5c, "little") if p5c else None
        print(f"   *(r0+0x5c)=0x{orig:08x} (list base)" if orig else
              f"   r0=0x{r0:08x} not in RAM region", flush=True)

        # drive interp until it passes the spin cycle, then sample
        print(f"==> driving interp until cyc>={rcyc}", flush=True)
        f = 0
        while cyc(intp) < rcyc and f < args.rec_frame + 40:
            f += 1; step(intp, f)
        icyc = cyc(intp); ir = regs(intp)
        print(f"   interp now cyc={icyc} (f≈{f}) r0=0x{ir['r0']:08x} "
              f"r1=0x{ir['r1']:08x}", flush=True)

        # the corrupt field itself: r0+0x5c (the command pointer the loop loads)
        fa = r0 + 0x5c
        rf = readmem(rec, fa, 4); intf = readmem(intp, fa, 4)
        if rf and intf:
            print(f"\n== FIELD r0+0x5c (0x{fa:08x}): recomp="
                  f"0x{int.from_bytes(rf,'little'):08x} interp="
                  f"0x{int.from_bytes(intf,'little'):08x}", flush=True)

        for label, base in (("list@r1", r1), ("list@orig", orig or r1),
                            ("obj@r0", r0), ("field@r0+0x5c", fa)):
            if region_cmd(base) is None:
                print(f"-- {label} 0x{base:08x}: not in IWRAM/EWRAM (ROM?) — "
                      f"identical by construction", flush=True)
                continue
            lo = (base - 0x40) & ~0xF
            a = readmem(rec, lo, args.win)
            b = readmem(intp, lo, args.win)
            print(f"\n-- {label}: window 0x{lo:08x}..0x{lo+args.win:08x}", flush=True)
            if a is None or b is None:
                print("   (read failed)"); continue
            diffs = [(lo + k, a[k], b[k]) for k in range(len(a)) if a[k] != b[k]]
            if not diffs:
                print("   IDENTICAL", flush=True)
            else:
                for off, x, y in diffs:
                    mark = " <== r1" if off == r1 else ""
                    print(f"   0x{off:08x}: recomp={x:02x} interp={y:02x}{mark}",
                          flush=True)
        for cl in (rec, intp):
            try: cl.call(cmd="quit")
            except Exception: pass
    finally:
        for p in (pr, pi):
            try: p.wait(timeout=8)
            except subprocess.TimeoutExpired: p.kill()


if __name__ == "__main__":
    sys.exit(main())
