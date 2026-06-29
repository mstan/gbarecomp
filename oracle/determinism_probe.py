#!/usr/bin/env python3
"""determinism_probe.py -- Axis 7 (Determinism) probe. Runs the recomp from COLD
to N frames TWICE and compares end-state; PASS iff bit-identical. Also runs the
NBA oracle twice as a control.

The only legitimate way to test determinism is two independent COLD runs (never a
savestate re-origin, never pause/step) -- so this tool SPAWNS the recomp process
twice, each from reset, advances it N whole frames, and snapshots:
  * state_hash  -> per-region FNV (iwram/ewram/vram/pal/oam) + combined hash
  * audio_cap   -> the always-on mixed-sample ring (hashed)
Two cold runs must match. A mismatch is nondeterminism; the most likely culprit
on GBA is the RTC host-time leak (gba_rtc.cpp std::time/localtime -- RSE/FRLG),
followed by the post-load PSG ch3/4 re-seed (not relevant to a cold run). The
per-region breakdown localizes which surface drifted.

NBA control: NBA is deterministic by construction; two `reset`+`run_frames` runs
are hashed over the same surfaces + audio and must match.

Reads (recomp): state_hash, audio_cap.   (nba): read{...}, audio_cap.
No comparator pauses/steps or aligns savestates -- cold runs only.

Usage
-----
  # real probe (MinishCap game build):
  python oracle/determinism_probe.py --frames 120
  # self-test stand-in (game build pending): use bios_smoke as the recomp exe
  python oracle/determinism_probe.py --frames 60 \
     --recomp-exe build/bios_smoke.exe \
     --bios bios/gba_bios.bin --rom ../MinishCapRecomp/roms/minishcap_usa.gba
  # NBA control only:
  python oracle/determinism_probe.py --frames 60 --nba-only
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys
import time

import nba_common as nc
import recomp_paths as rp

ROOT = pathlib.Path(__file__).resolve().parent.parent


def _h(*parts):
    m = hashlib.sha1()
    for p in parts:
        if p is None:
            p = b"<none>"
        if isinstance(p, str):
            p = p.encode()
        m.update(p)
        m.update(b"|")
    return m.hexdigest()


def snapshot_recomp(c):
    sh = c.call(cmd="state_hash")
    if not nc.ok(sh):
        return None
    regions = {k: sh.get(k) for k in ("iwram", "ewram", "vram", "pal", "oam")}
    # Fixed absolute window from boot sample 0 -- determinism-stable (a most-
    # recent window shifts if the two runs' sample heads differ by even one).
    ac = c.call(cmd="audio_cap", start=0, count=40000)
    audio = ac.get("mixed", "") if nc.ok(ac) else ""
    regions["audio"] = _h(audio)
    regions["combined"] = sh.get("hash")
    return regions


def snapshot_nba(c):
    parts = []
    regs = {}
    for name in ("vram", "pal", "oam", "iwram", "ewram"):
        b = nc.read_region_nba(c, name)
        regs[name] = _h(b)
        parts.append(b or b"")
    ac = c.call(cmd="audio_cap", start=0, count=40000)
    if nc.ok(ac):
        a = nc.hx(ac.get("l", "")) + nc.hx(ac.get("r", ""))
    else:
        a = b""
    regs["audio"] = _h(a)
    regs["combined"] = _h(*parts, a)
    return regs


def diff_snaps(s1, s2, label):
    print(f"\n=== {label} ===")
    if s1 is None or s2 is None:
        print("  ! a run produced no snapshot (server/command unavailable)")
        return "ERR"
    keys = [k for k in ("iwram", "ewram", "vram", "pal", "oam", "audio",
                        "combined") if k in s1 or k in s2]
    allmatch = True
    for k in keys:
        a, b = s1.get(k), s2.get(k)
        same = a == b
        allmatch &= same
        flag = "OK " if same else "DIFFER"
        print(f"  {k:<9}: {flag}  run1={a} run2={b}")
    v = "DETERMINISTIC" if allmatch else "NONDETERMINISTIC"
    print(f"  VERDICT: {v}")
    if not allmatch:
        print("  likely cause: RTC host-time leak (gba_rtc.cpp std::time/"
              "localtime) if this game reads RTC; else investigate the DIFFERing "
              "surface above.")
    return v


def spawn_recomp(exe, bios, rom, port):
    cmd = [str(exe)]
    if bios:
        cmd += ["--bios", str(bios)]
    if rom:
        cmd += ["--rom", str(rom)]
    cmd += ["--tcp", str(port)]
    p = subprocess.Popen(cmd, cwd=str(ROOT),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return p


def run_recomp_once(exe, bios, rom, port, frames, host):
    p = spawn_recomp(exe, bios, rom, port)
    try:
        c = nc.connect(host, port, "recomp", connect_deadline=12.0)
        if not c:
            return None
        for _ in range(frames):
            try:
                c.call(timeout=30, cmd="step")
            except Exception as e:
                print(f"[warn] recomp step failed/stalled: {e}")
                break
        snap = snapshot_recomp(c)
        try:
            c.call(cmd="quit")
        except Exception:
            pass
        c.close()
        return snap
    finally:
        try:
            p.wait(timeout=6)
        except subprocess.TimeoutExpired:
            p.kill()


def run_nba_twice(c, frames):
    snaps = []
    for _ in range(2):
        c.call(cmd="reset")
        c.call(cmd="run_frames", n=frames)
        snaps.append(snapshot_nba(c))
    return snaps


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--recomp-exe", default=None,
                    help="recomp executable (default: resolved game build)")
    ap.add_argument("--bios", default=None, help="--bios arg (bios_smoke only)")
    ap.add_argument("--rom", default=None, help="--rom arg (bios_smoke only)")
    ap.add_argument("--recomp-port", type=int, default=19842)
    ap.add_argument("--nba-port", type=int, default=19844)
    ap.add_argument("--nba-only", action="store_true")
    ap.add_argument("--recomp-only", action="store_true")
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()

    rc = nrc = None

    if not args.nba_only:
        exe = pathlib.Path(args.recomp_exe) if args.recomp_exe else rp.recomp_exe(ROOT)
        if not exe.exists():
            print(f"[skip] recomp exe not found: {exe}\n"
                  f"       (the MinishCap game build is still compiling -- pass "
                  f"--recomp-exe build/bios_smoke.exe with --bios/--rom to self-"
                  f"test the probe now, or --nba-only for the control.)")
        else:
            print(f"[recomp] cold run 1/2: {exe} ({args.frames} frames) ...")
            s1 = run_recomp_once(exe, args.bios, args.rom, args.recomp_port,
                                 args.frames, args.host)
            print(f"[recomp] cold run 2/2 ...")
            s2 = run_recomp_once(exe, args.bios, args.rom, args.recomp_port,
                                 args.frames, args.host)
            rc = diff_snaps(s1, s2, "RECOMP DETERMINISM (cold x2)")

    if not args.recomp_only:
        nba = nc.connect(args.host, args.nba_port, "nba")
        if nba:
            print(f"\n[nba] control: cold x2, {args.frames} frames ...")
            n1, n2 = run_nba_twice(nba, args.frames)
            nba.close()
            nrc = diff_snaps(n1, n2, "NBA DETERMINISM CONTROL (cold x2)")
        else:
            print("[skip] nba control unavailable")

    fail = (rc == "NONDETERMINISTIC") or (nrc == "NONDETERMINISTIC")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
