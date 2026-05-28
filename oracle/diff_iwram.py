#!/usr/bin/env python3
"""diff_iwram.py — find the EARLIEST recomp-vs-interpreter divergence in
full memory, phase-aligned at VBlank-start.

The handoff's diff_anim watched only entity+0x12 and gPriorityHandler AND
compared the recomp (parked at scanline-wrap) against the interpreter
(parked at VBlank-start) — two different PPU moments — which manufactured
a spurious "recomp runs a frame ahead". The recomp `step` now parks at
VBlank-start too (g_runtime_vblank_starts), so both sides sit at the same
PPU phase and the same frame number at each step.

This tool steps both in lockstep and diffs the FULL 32 KB IWRAM (and,
with --ewram, the 256 KB EWRAM) each frame, reporting the FIRST diverging
byte and the frame it appears — the real lead per the SYNC RULES
("the earliest divergence is the only one with a root cause").

Usage:
    python oracle/diff_iwram.py [--state P] [--frames N] [--hold up]
                                [--ewram] [--keep-going] [--window 64]
"""
from __future__ import annotations
import argparse, json, pathlib, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJ = ROOT.parent / "MinishCapRecomp"
RECOMP_EXE = PROJ / "build" / "MinishCapRecomp.exe"
INTERP_EXE = ROOT / "build" / "bios_smoke.exe"
BIOS = ROOT / "bios" / "gba_bios.bin"
ROM = PROJ / "roms" / "minishcap_usa.gba"
DEF_STATE = PROJ / "roms" / "minishcap_usa.state3"

KEYMASK = {"up": 0x3FF & ~0x40, "down": 0x3FF & ~0x80,
           "left": 0x3FF & ~0x20, "right": 0x3FF & ~0x10, "none": 0x3FF}

# Known IWRAM/EWRAM globals for annotating a diverging address.
GLOBALS = [
    (0x03000000, "iwram_base"),
    (0x03001000, "gMain"),
    (0x030010A0, "gRoomTransition?"),
    (0x030015A0, "gEntities"),
    (0x030018D0, "gEntities[slot@18D0] (spin entity)"),
    (0x03003DC0, "gPriorityHandler"),
    (0x02000050, "gMessage(EWRAM)"),
]


def annotate(addr):
    best = None
    for a, n in GLOBALS:
        if a <= addr and (best is None or a > best[0]):
            best = (a, n)
    if best:
        return f"{best[1]}+0x{addr - best[0]:x}"
    return "?"


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
                ch = self.s.recv(1 << 20)
                if not ch:
                    raise RuntimeError("closed")
                self.b += ch
        finally:
            self.s.settimeout(None)
        line, _, self.b = self.b.partition(b"\n")
        return json.loads(line.decode())

    def read(self, cmd, base, n):
        r = self.call(cmd=cmd, addr=base, len=n)
        if not r.get("ok"):
            raise RuntimeError(f"{cmd}@{base:#x}: {r}")
        return bytes.fromhex(r["data"])


def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return None if len(a) == len(b) else n


def all_diffs(a, b, limit=24):
    out = []
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            out.append(i)
            if len(out) >= limit:
                break
    return out


def clusters(offsets, gap=8):
    """Group sorted offsets into [lo,hi] ranges separated by > gap."""
    out = []
    for o in offsets:
        if out and o - out[-1][1] <= gap:
            out[-1][1] = o
        else:
            out.append([o, o])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default=str(DEF_STATE))
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--hold", default="up")
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--ewram", action="store_true")
    ap.add_argument("--keep-going", action="store_true",
                    help="don't stop at first divergence")
    ap.add_argument("--window", type=int, default=48)
    ap.add_argument("--detail-at", type=int, default=-1,
                    help="dump clustered diff + hex windows at this frame")
    ap.add_argument("--ignore-irqstack", action="store_true",
                    help="ignore IWRAM >= 0x7e00 (BIOS IRQ stack / scratch, "
                         "known VBlank-handler sample noise) when locating "
                         "the first real divergence")
    ap.add_argument("--no-spawn", action="store_true")
    ap.add_argument("--align-irq", action="store_true",
                    help="after each step, advance each side through its "
                         "VBlank IRQ handler (step_inst until CPSR leaves IRQ "
                         "mode) so both park post-handler at the same "
                         "execution point — removes the IRQ sample-offset.")
    args = ap.parse_args()
    mask = KEYMASK.get(args.hold.lower(), 0x3FF)

    def align(cl):
        # If parked in IRQ mode (0x12), step instructions until the handler
        # returns (mode leaves IRQ). Bounded to avoid runaway.
        for _ in range(20000):
            r = cl.call(cmd="registers")
            if (r.get("cpsr", 0) & 0x1F) != 0x12:
                return
            cl.call(cmd="step_inst")

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
        rec, intp = C(19842), C(19844)
        print("==> connected recomp=19842 interp=19844", flush=True)
        for who, cl in (("recomp", rec), ("interp", intp)):
            r = cl.call(cmd="savestate_load", path=args.state)
            if not r.get("ok"):
                print(f"{who} load failed: {r}"); return 1
            cl.call(cmd="set_keyinput", value=mask)

        regions = [("read_iwram", 0x03000000, 0x8000, "IWRAM")]
        if args.ewram:
            regions.append(("read_ewram", 0x02000000, 0x40000, "EWRAM"))

        # IWRAM offset >= this is the BIOS IRQ stack / scratch — noisy
        # VBlank-handler residue, not game state.
        IRQ_NOISE = 0x7e00

        def hexwin(base, rb, ib, fd):
            lo = max(0, fd - 8) & ~0xF
            hi = min(len(rb), fd + args.window)
            print(f"  region {base+lo:#08x}..{base+hi:#08x}:", flush=True)
            for o in range(lo, hi, 16):
                rr = rb[o:o+16]; ii = ib[o:o+16]
                mark = "".join("^^" if rr[k] != ii[k] else "  "
                               for k in range(len(rr)))
                print(f"    {base+o:#08x} R {rr.hex()}", flush=True)
                print(f"               I {ii.hex()}", flush=True)
                print(f"                 {mark}", flush=True)

        rec_dead = False
        found = False
        for f in range(0, args.frames + 1):
            line = [f"f{f:3d}:"]
            diverged_here = None
            for cmd, base, size, tag in regions:
                if rec_dead:
                    line.append(f"{tag}=(recomp spun)")
                    continue
                rb = rec.read(cmd, base, size)
                ib = intp.read(cmd, base, size)
                offs = all_diffs(rb, ib, 100000)
                real = [o for o in offs
                        if not (args.ignore_irqstack and base == 0x03000000
                                and o >= IRQ_NOISE)]
                if not offs:
                    line.append(f"{tag}=identical")
                else:
                    cl_all = clusters(offs)
                    fd = real[0] if real else offs[0]
                    addr = base + fd
                    line.append(f"{tag}=DIFF@{addr:#08x}({annotate(addr)}) "
                                f"n={len(offs)} clusters={len(cl_all)}")
                    if real and diverged_here is None and not found:
                        diverged_here = (tag, base, real[0], rb, ib)
                    if f == args.detail_at:
                        print(" ".join(line), flush=True)
                        print(f"  [detail @f{f}] {tag} clusters "
                              f"(real game state, IRQ-stack excluded):",
                              flush=True)
                        for lo_, hi_ in cl_all:
                            if lo_ >= IRQ_NOISE and base == 0x03000000:
                                continue
                            print(f"    {base+lo_:#08x}..{base+hi_:#08x} "
                                  f"({annotate(base+lo_)}, {hi_-lo_+1}B)",
                                  flush=True)
                        if real:
                            hexwin(base, rb, ib, real[0])
            print(" ".join(line), flush=True)

            if diverged_here and not found:
                found = True
                tag, base, fd, rb, ib = diverged_here
                addr = base + fd
                print(f"\n===== FIRST REAL {tag} DIVERGENCE at frame {f} "
                      f"@ {addr:#08x} ({annotate(addr)}) =====", flush=True)
                offs = [o for o in all_diffs(rb, ib, 100000)
                        if not (args.ignore_irqstack and base == 0x03000000
                                and o >= IRQ_NOISE)]
                print(f"  real diverging clusters: "
                      f"{[(hex(base+a),hex(base+b)) for a,b in clusters(offs)]}",
                      flush=True)
                hexwin(base, rb, ib, fd)
                if not args.keep_going:
                    print("  (stop; pass --keep-going to watch the cascade)",
                          flush=True)
                    break

            # step both
            if not rec_dead:
                try:
                    rec.call(timeout=args.timeout, cmd="step")
                    if args.align_irq:
                        align(rec)
                except socket.timeout:
                    print(f"   recomp SPUN at frame {f} (step timeout)",
                          flush=True)
                    rec_dead = True
            intp.call(timeout=args.timeout, cmd="step")
            if args.align_irq:
                align(intp)
            if rec_dead:
                print("   recomp spun; interp alive -> RECOMP BUG", flush=True)
                break

        if not found and not rec_dead:
            print("\n==> NO IWRAM divergence across the run "
                  "(recomp == interpreter).", flush=True)
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
