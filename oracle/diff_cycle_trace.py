#!/usr/bin/env python3
"""diff_cycle_trace.py — MC-HP-002: find the FIRST real divergence between the
recomp and the interpreter oracle, cycle-aligned, via the always-on
per-instruction fingerprint ring (NOT pause/step lockstep — free-run + diff).

FRESH RUN, NO SAVESTATES. Both engines boot their own BIOS from reset and run the
SAME ROM with the SAME input schedule (the diff_intro.py START pulse that drives
through the title into the stained-glass intro, where the recomp spins ~f3645).
Booting fresh means both cycle clocks start at 0 at reset and stay comparable —
no savestate to re-origin, and no risk of a desynced savestate manufacturing a
false divergence.

When launched with GBARECOMP_INSN_TRACE=1, each engine records a fingerprint of
EVERY executed instruction into a bounded ~1M-entry ring: {cumulative guest cycles,
pc, packed cpsr, R0..R15}. The recomp's clock is g_runtime_cycles (runtime_tick,
counts exec + halt-pump); the interp's is bios_smoke's cycles_elapsed.

We free-run both to --to-frame (default just shy of the spin), dump each ring to a
GFP1 binary file, then anchor the two streams at a verified-common instruction and
walk forward. The FIRST fingerprint that differs (pc, any GPR, or cpsr) is the
first real divergence — the recompiled instruction + data to audit against ROM.
If the recomp SPINS before --to-frame, lower it; the ring only holds ~8 frames, so
--to-frame must land within ~8 frames after the divergence and before the spin.

Usage:
    python oracle/diff_cycle_trace.py --to-frame 3643          # near the f3645 spin
    python oracle/diff_cycle_trace.py --to-frame 3643 --ctx 24
"""
from __future__ import annotations
import argparse, json, os, pathlib, socket, struct, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent          # gbarecomp/
PROJ = ROOT.parent / "MinishCapRecomp"
RECOMP_EXE = PROJ / "build" / "MinishCapRecomp.exe"
INTERP_EXE = ROOT / "build" / "bios_smoke.exe"
BIOS = ROOT / "bios" / "gba_bios.bin"
ROM = PROJ / "roms" / "minishcap_usa.gba"
OUT = ROOT / "oracle" / "trace_out"

# Input schedule, identical to diff_intro.py: pulse START (active-low bit 3)
# from frame 200 on, to drive through the title into the stained-glass intro.
START_KEYINPUT = 0x3F7
NONE_KEYINPUT = 0x3FF


def held_this_frame(f: int) -> bool:
    return f >= 200 and (f // 6) % 6 == 0

# GFP1 record: u64 cycles, u32 pc, u32 cpsr, 16 * u32 regs  → 80 bytes.
REC = struct.Struct("<QII16I")
HDR = struct.Struct("<IIQ")  # magic 'GFP1', entry_size, count


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

    def read_iwram(self):
        """Full 32 KB IWRAM as bytes (read_iwram, 4 KB chunks)."""
        base, size, chunk = 0x03000000, 32 * 1024, 4096
        out = bytearray()
        for off in range(0, size, chunk):
            r = self.call(cmd="read_iwram", addr=base + off, len=chunk)
            if not r.get("ok"):
                raise RuntimeError(f"read_iwram @{base+off:#x}: {r}")
            out += bytes.fromhex(r["data"])
        return bytes(out)


def load_fp(path: pathlib.Path):
    """Read a GFP1 file → list of (cycles, pc, cpsr, [r0..r15]) oldest-first."""
    data = path.read_bytes()
    magic, esz, count = HDR.unpack_from(data, 0)
    if magic != 0x31504647:
        raise RuntimeError(f"{path}: bad magic 0x{magic:08x}")
    if esz != REC.size:
        raise RuntimeError(f"{path}: entry size {esz} != {REC.size}")
    off = HDR.size
    out = []
    for _ in range(count):
        f = REC.unpack_from(data, off); off += REC.size
        out.append((f[0], f[1], f[2], list(f[3:])))
    return out


def arch(e):
    """Architectural state of a fingerprint: (pc, cpsr, regs) — cycle excluded.
    This is what we diff: a pure cycle-count difference is timing, not a real
    divergence, so cycles are alignment/info only (the recomp and interp cycle
    models are consistent through f39 but we don't want to depend on bit-exact
    cumulative cycles to define 'divergence')."""
    return (e[1], e[2], tuple(e[3]))


def anchor(rec, intp):
    """Align the two rings at the SAME instruction. Pre-divergence both engines
    execute the identical stream, so recomp's oldest fingerprint also appears in
    interp; we match it by full architectural state (near-unique with 16 regs),
    breaking ties by closest cumulative cycle. Returns (i0, j0) or (None, None)."""
    target = arch(rec[0])
    cand = [j for j, e in enumerate(intp) if arch(e) == target]
    if not cand:
        return None, None
    j0 = min(cand, key=lambda j: abs(intp[j][0] - rec[0][0]))
    return 0, j0


def fmt(e):
    regs = " ".join(f"r{k}={e[3][k]:08x}" for k in range(16))
    return f"cyc={e[0]:<10} pc={e[1]:08x} cpsr={e[2]:08x} {regs}"


def diff(rec, intp, ctx):
    i0, j0 = anchor(rec, intp)
    if i0 is None:
        print("!! recomp's oldest fingerprint has no architectural match in the "
              "interp ring — the rings don't overlap. Lower --to-frame (recomp "
              "ring may have flooded) or enlarge kFpSize.", flush=True)
        return 1
    print(f"    recomp span: {len(rec)} fps, cyc {rec[0][0]}..{rec[-1][0]} "
          f"pc {rec[0][1]:08x}..{rec[-1][1]:08x}", flush=True)
    print(f"    interp span: {len(intp)} fps, cyc {intp[0][0]}..{intp[-1][0]} "
          f"pc {intp[0][1]:08x}..{intp[-1][1]:08x}", flush=True)
    cyc_skew = rec[i0][0] - intp[j0][0]
    print(f"==> anchored at recomp idx {i0} / interp idx {j0} "
          f"(pc={rec[i0][1]:08x}, cycle skew {cyc_skew:+d}); "
          f"recomp ring={len(rec)} fps, interp ring={len(intp)} fps", flush=True)
    i, j, n = i0, j0, 0
    while i < len(rec) and j < len(intp):
        a, b = rec[i], intp[j]
        if arch(a) != arch(b):
            print("\n*** FIRST DIVERGENCE ***", flush=True)
            lo = max(0, n - ctx)
            for k in range(lo, n):  # aligned, verified-identical context
                print(f"  [-{n-k:>3}] OK  {fmt(rec[i-(n-k)])}")
            print(f"  recomp: {fmt(a)}")
            print(f"  interp: {fmt(b)}")
            difs = []
            if a[1] != b[1]: difs.append(f"pc {a[1]:08x}!={b[1]:08x}")
            if a[2] != b[2]: difs.append(f"cpsr {a[2]:08x}!={b[2]:08x}")
            for k in range(16):
                if a[3][k] != b[3][k]:
                    difs.append(f"r{k} {a[3][k]:08x}!={b[3][k]:08x}")
            print("  DIFF:", ", ".join(difs), flush=True)
            if a[0] != b[0]:
                print(f"  (cumulative-cycle at divergence: recomp {a[0]} vs "
                      f"interp {b[0]}, skew {a[0]-b[0]:+d})", flush=True)
            return 0
        i += 1; j += 1; n += 1
    # One ring ended before any architectural mismatch. If the INTERP ended while
    # the recomp keeps going, the interp halted (idle) where the recomp did not —
    # that boundary IS the divergence (a halt / IRQ / SWI-wait handling bug).
    if j >= len(intp) and i < len(rec):
        print(f"\n*** INTERP STREAM ENDED at aligned instr {n} while the recomp "
              f"continues ({len(rec)-i} more fps) — the interp HALTED here and the "
              f"recomp did NOT. ***", flush=True)
        print("  last aligned (both identical):", flush=True)
        for k in range(max(i0, i - 4), i):
            print(f"    {fmt(rec[k])}")
        print(f"  interp's final fingerprint: {fmt(intp[len(intp)-1])}", flush=True)
        print("  recomp continues past the halt with:", flush=True)
        for k in range(i, min(len(rec), i + 24)):
            print(f"    {fmt(rec[k])}")
        return 0
    print(f"==> no architectural divergence across {n} aligned instructions "
          f"(identical in the overlap). Increase --until / widen the window.",
          flush=True)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--diff-from", type=int, default=3400,
                    help="start per-frame IWRAM comparison at this frame (lockstep "
                         "silently before it). Lower if the divergence is earlier.")
    ap.add_argument("--until", type=int, default=3700,
                    help="give up if no IWRAM divergence by this frame")
    ap.add_argument("--scan-step", type=int, default=0,
                    help="coarse-scan mode: check IWRAM every N frames and report the "
                         "first diverging window, then exit (no fingerprint dump). Use "
                         "to bracket the divergence, then re-run fine (--scan-step 0) "
                         "with --diff-from just below the bracket.")
    ap.add_argument("--ctx", type=int, default=16,
                    help="aligned instructions of context before the divergence")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--no-spawn", action="store_true")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    rec_fp = OUT / "recomp_fp.bin"
    intp_fp = OUT / "interp_fp.bin"

    procs = []
    try:
        if not args.no_spawn:
            for exe in (RECOMP_EXE, INTERP_EXE):
                if not exe.exists():
                    print(f"missing {exe}", file=sys.stderr); return 1
            env = dict(os.environ, GBARECOMP_INSN_TRACE="1")
            procs.append(subprocess.Popen(
                [str(RECOMP_EXE), "--tcp", "19842"], cwd=str(PROJ), env=env,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            procs.append(subprocess.Popen(
                [str(INTERP_EXE), "--bios", str(BIOS), "--rom", str(ROM),
                 "--tcp", "19844"], cwd=str(ROOT), env=env,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

        rec = JsonClient("127.0.0.1", 19842)
        intp = JsonClient("127.0.0.1", 19844)
        print("==> connected: recomp=19842 interp=19844 (fresh boot, insn-trace armed)",
              flush=True)
        print(f"==> driving the intro (START pulse); IWRAM check from f{args.diff_from}",
              flush=True)

        prev_ckpt = None  # last frame confirmed IWRAM-identical (coarse mode)
        div_frame = None
        for f in range(1, args.until + 1):
            mask = START_KEYINPUT if held_this_frame(f) else NONE_KEYINPUT
            rec.call(cmd="set_keyinput", value=mask)
            intp.call(cmd="set_keyinput", value=mask)
            try:
                rec.call(timeout=args.timeout, cmd="step")
            except socket.timeout:
                print(f"\n!! recomp SPUN at frame {f} before any IWRAM divergence "
                      f"was seen — lower --diff-from to catch it earlier.", flush=True)
                return 2
            intp.call(timeout=args.timeout, cmd="step")
            if f < args.diff_from:
                if f % 500 == 0:
                    print(f"  ...lockstep frame {f}", flush=True)
                continue
            # In coarse-scan mode only sample every --scan-step frames.
            if args.scan_step and (f - args.diff_from) % args.scan_step != 0:
                continue
            niw = rec.read_iwram()
            oiw = intp.read_iwram()
            if niw != oiw:
                ndiff = sum(1 for a, b in zip(niw, oiw) if a != b)
                first = next(i for i, (a, b) in enumerate(zip(niw, oiw)) if a != b)
                print(f"== frame {f}: IWRAM diverged ({ndiff} bytes; first @"
                      f"0x{0x03000000+first:08x})", flush=True)
                if args.scan_step:
                    lo = prev_ckpt + 1 if prev_ckpt is not None else args.diff_from
                    print(f"\n==> divergence is in the window [f{lo} .. f{f}]. "
                          f"Re-run fine: --scan-step 0 --diff-from {max(1, lo-1)}",
                          flush=True)
                    for cl in (rec, intp):
                        try: cl.call(cmd="quit")
                        except Exception: pass
                    return 0
                div_frame = f
                break
            prev_ckpt = f
            if f % (args.scan_step or 25) == 0:
                print(f"  ...f{f} IWRAM identical", flush=True)

        if div_frame is None:
            print(f"==> no IWRAM divergence through f{args.until}. The bug may be "
                  f"register/EWRAM-only, or later — raise --until or widen the probe.",
                  flush=True)
            return 0

        print("==> dumping fingerprint rings at the divergence frame", flush=True)
        rr = rec.call(cmd="fp_save", path=str(rec_fp))
        ir = intp.call(cmd="fp_save", path=str(intp_fp))
        print(f"    recomp wrote {rr.get('count')} fps; interp wrote {ir.get('count')}",
              flush=True)
        for cl in (rec, intp):
            try: cl.call(cmd="quit")
            except Exception: pass

        if not rr.get("count") or not ir.get("count"):
            print("!! a ring was empty — was GBARECOMP_INSN_TRACE armed?", flush=True)
            return 1
        return diff(load_fp(rec_fp), load_fp(intp_fp), args.ctx)
    finally:
        for p in procs:
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill()


if __name__ == "__main__":
    sys.exit(main())
