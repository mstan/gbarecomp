#!/usr/bin/env python3
"""fp_cycle_diff.py — cycle-aligned instruction diff between the recomp and the
SAVE-MATCHED interp oracle, done the RIGHT way: compare (cycle, pc, cpsr, regs)
including the CYCLE. The older diff_cycle_trace.py excluded cycle from its anchor
walk, so an instruction-stream phase offset (e.g. a HALT/IntrWait flag-wait that
spins a different number of times) slid through and produced FALSE divergences
(the key-repeat phantom). With matched saves (GBARECOMP_LOAD_SAV=1) and the
cycle-accurate clock (kIrqWakeDelayCycles fix), both engines execute the identical
instruction stream at identical cumulative cycles until the REAL divergence — so
the first index where (cycle,pc,cpsr,regs) differ is it.

Drives the diff_intro START pulse to --frame on both (INSN_TRACE armed), dumps
each fp ring via the TCP fp_save command, and walks them anchored at a common
(cycle,pc). The fp ring holds ~8 frames, so --frame must be within ~8 of the
divergence (MC-HP-002: divergence at f682, so --frame 682).

  GBARECOMP_LOAD_SAV=1 GBARECOMP_RECOMP_EXE=.../build-selfheal/MinishCapRecomp.exe \
  python oracle/fp_cycle_diff.py --frame 682 --ctx 20
"""
from __future__ import annotations
import argparse, json, os, pathlib, socket, struct, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJ = ROOT.parent / "MinishCapRecomp"
RECOMP_EXE = pathlib.Path(os.environ.get(
    "GBARECOMP_RECOMP_EXE", PROJ / "build" / "MinishCapRecomp.exe"))
INTERP_EXE = pathlib.Path(os.environ.get(
    "GBARECOMP_INTERP_EXE", ROOT / "build" / "bios_smoke.exe"))
BIOS = ROOT / "bios" / "gba_bios.bin"
ROM = PROJ / "roms" / "minishcap_usa.gba"
OUT = ROOT / "oracle" / "trace_out"
START_KEYINPUT = 0x3F7
NONE_KEYINPUT = 0x3FF
REC = struct.Struct("<QII16I")     # u64 cycles, u32 pc, u32 cpsr, 16*u32 regs
HDR = struct.Struct("<IIQ")


FRAME_CYC = 280896  # GbaPpu::kCyclesPerFrame


def held(f):
    return f >= 200 and (f // 6) % 6 == 0


def held_cycle(c):
    f = c // FRAME_CYC
    return f >= 200 and (f // 6) % 6 == 0


def input_mask(f, mode, cyc=0):
    # 'pulse' = the original frame-indexed START spam (causes cycle-vs-frame
    # input jitter once the two engines run at different cycles/frame: the boot
    # step_frame overshoot offsets the two frame indices at equal cycle).
    # 'cycle' = same toggle pattern keyed on each engine's cumulative cycle, so
    # both see identical input at identical cycle — any surviving divergence is
    # a REAL bug, not input timing. 'hold'/'none' = constant input.
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


def load_fp(path):
    data = path.read_bytes()
    magic, esz, count = HDR.unpack_from(data, 0)
    assert magic == 0x31504647 and esz == REC.size, (magic, esz)
    off = HDR.size
    out = []
    for _ in range(count):
        f = REC.unpack_from(data, off); off += REC.size
        out.append((f[0], f[1], f[2], f[3:]))   # (cycles, pc, cpsr, regs)
    return out


def fmt(e):
    return (f"cyc={e[0]:<11} pc={e[1]:08x} cpsr={e[2]:08x} " +
            " ".join(f"r{k}={e[3][k]:08x}" for k in range(16)))


def diff(rec, intp, ctx):
    # Both rings are dumped chronologically (oldest-first), so cycles ascend.
    # Two-pointer merge by cycle: advance into the common cycle window, then walk
    # in lockstep. While the engines are cycle-locked, each shared cycle carries
    # an identical entry. The FIRST divergence is either (a) a shared cycle whose
    # entries differ, or (b) the first cycle present in only one ring after the
    # overlap began (instruction-boundary divergence). O(n) time, O(1) space —
    # scales to the 8M-entry ring without the multi-GB dict the old anchor built.
    i, j = 0, 0
    nr, ni = len(rec), len(intp)
    lo = max(rec[0][0], intp[0][0])
    hi = min(rec[-1][0], intp[-1][0])
    if hi < lo:
        print(f"!! rings don't overlap in cycle "
              f"[rec {rec[0][0]}..{rec[-1][0]}] vs "
              f"[int {intp[0][0]}..{intp[-1][0]}]. Adjust --frame/--interp-lead.",
              flush=True)
        return 1
    while i < nr and rec[i][0] < lo: i += 1
    while j < ni and intp[j][0] < lo: j += 1
    print(f"==> overlap cyc[{lo}..{hi}]; recomp {nr} fps, interp {ni} fps; "
          f"start recomp[{i}] interp[{j}]", flush=True)
    matched = []   # recent matched recomp entries for context
    n = 0
    while i < nr and j < ni and rec[i][0] <= hi and intp[j][0] <= hi:
        a, b = rec[i], intp[j]
        if a[0] == b[0]:
            if a != b:
                print("\n*** FIRST DIVERGENCE (cycle-aligned) ***", flush=True)
                for e in matched[-ctx:]:
                    print(f"  OK     {fmt(e)}")
                print(f"  recomp: {fmt(a)}")
                print(f"  interp: {fmt(b)}")
                d = []
                if a[1] != b[1]: d.append(f"pc {a[1]:08x}!={b[1]:08x}")
                if a[2] != b[2]: d.append(f"cpsr {a[2]:08x}!={b[2]:08x}")
                for k in range(16):
                    if a[3][k] != b[3][k]:
                        d.append(f"r{k} {a[3][k]:08x}!={b[3][k]:08x}")
                print("  DIFF:", ", ".join(d), flush=True)
                return 0
            matched.append(a)
            if len(matched) > ctx + 2: matched.pop(0)
            i += 1; j += 1; n += 1
        elif a[0] < b[0]:
            # recomp has a cycle the interp skipped → boundary divergence
            print("\n*** FIRST DIVERGENCE (instruction boundary) ***", flush=True)
            for e in matched[-ctx:]:
                print(f"  OK     {fmt(e)}")
            print(f"  recomp has cyc={a[0]} (pc={a[1]:08x}) with no interp entry; "
                  f"next interp cyc={b[0]} pc={b[1]:08x}", flush=True)
            print(f"  recomp: {fmt(a)}")
            print(f"  interp: {fmt(b)}", flush=True)
            return 0
        else:
            print("\n*** FIRST DIVERGENCE (instruction boundary) ***", flush=True)
            for e in matched[-ctx:]:
                print(f"  OK     {fmt(e)}")
            print(f"  interp has cyc={b[0]} (pc={b[1]:08x}) with no recomp entry; "
                  f"next recomp cyc={a[0]} pc={a[1]:08x}", flush=True)
            print(f"  recomp: {fmt(a)}")
            print(f"  interp: {fmt(b)}", flush=True)
            return 0
    print(f"==> no divergence across {n} aligned cycles (identical overlap "
          f"cyc[{lo}..{hi}]); raise --frame.", flush=True)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frame", type=int, default=682)
    ap.add_argument("--ctx", type=int, default=20)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--input", choices=("pulse", "hold", "none", "cycle"),
                    default="pulse",
                    help="input schedule: pulse=frame-indexed START spam (jitter); "
                         "cycle=same pattern keyed on cumulative cycle (no jitter); "
                         "hold/none=cycle-independent constant input")
    ap.add_argument("--interp-lead", type=int, default=0,
                    help="drive the interp this many frames PAST the recomp so its "
                         "ring reaches the recomp's (cycle-ahead) divergence point")
    ap.add_argument("--break-pc", type=lambda s: int(s, 0), default=0,
                    help="set_break_pc on the RECOMP so a busy-spin at this PC "
                         "unwinds the dispatch (step returns) — lets the ring be "
                         "dumped at the spin entry instead of hanging the step")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    rfp, ifp = OUT / "rec_fp.bin", OUT / "int_fp.bin"
    env = dict(os.environ, GBARECOMP_INSN_TRACE="1")
    procs = [
        subprocess.Popen([str(RECOMP_EXE), "--tcp", "19842"], cwd=str(PROJ), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
        subprocess.Popen([str(INTERP_EXE), "--bios", str(BIOS), "--rom", str(ROM),
                          "--tcp", "19844"], cwd=str(ROOT), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
    ]
    try:
        rec, intp = C(19842), C(19844)
        print(f"==> driving both to f{args.frame} (START pulse, insn-trace)", flush=True)
        if args.break_pc:
            rec.call(cmd="set_break_pc", value=args.break_pc)
            print(f"    recomp break_pc=0x{args.break_pc:08x}", flush=True)
        def cyc_of(cl):
            try:
                return cl.call(cmd="counters").get("cycles_elapsed", 0)
            except Exception:
                return 0

        ilast = args.frame + max(0, args.interp_lead)
        for f in range(1, ilast + 1):
            if f <= args.frame:
                rmask = input_mask(f, args.input,
                                   cyc_of(rec) if args.input == "cycle" else 0)
                rec.call(cmd="set_keyinput", value=rmask)
                rec.call(timeout=args.timeout, cmd="step")
            imask = input_mask(f, args.input,
                               cyc_of(intp) if args.input == "cycle" else 0)
            intp.call(cmd="set_keyinput", value=imask)
            intp.call(timeout=args.timeout, cmd="step")
        rr = rec.call(cmd="fp_save", path=str(rfp))
        ir = intp.call(cmd="fp_save", path=str(ifp))
        print(f"    recomp {rr.get('count')} fps, interp {ir.get('count')} fps",
              flush=True)
        for cl in (rec, intp):
            try: cl.call(cmd="quit")
            except Exception: pass
        return diff(load_fp(rfp), load_fp(ifp), args.ctx)
    finally:
        for p in procs:
            try: p.wait(timeout=8)
            except subprocess.TimeoutExpired: p.kill()


if __name__ == "__main__":
    sys.exit(main())
