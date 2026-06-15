#!/usr/bin/env python3
"""mem_diff.py — frame-aligned IWRAM+EWRAM diff between recomp and the save-matched
interp oracle, at a chosen frame. LEGITIMATE here (unlike a different-path diff)
because with matched saves (GBARECOMP_LOAD_SAV=1) the engines are on the SAME game
path and bit-identical through the frame before the divergence — so a diff at the
first diverging frame names the exact diverging region (per recomp-template, this
is only valid when both sides are at the same game moment).

Drives the diff_intro START pulse to --frame on both, reads IWRAM (32K) and, with
--ewram, EWRAM (256K), and reports the first diverging clusters (IRQ-stack scratch
>= 0x03007e00 excluded as known VBlank-handler noise).

  GBARECOMP_LOAD_SAV=1 GBARECOMP_RECOMP_EXE=.../build-selfheal/MinishCapRecomp.exe \
  python oracle/mem_diff.py --frame 682 --ewram
"""
from __future__ import annotations
import argparse, json, os, pathlib, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJ = ROOT.parent / "MinishCapRecomp"
RECOMP_EXE = pathlib.Path(os.environ.get(
    "GBARECOMP_RECOMP_EXE", PROJ / "build" / "MinishCapRecomp.exe"))
INTERP_EXE = pathlib.Path(os.environ.get(
    "GBARECOMP_INTERP_EXE", ROOT / "build" / "bios_smoke.exe"))
BIOS = ROOT / "bios" / "gba_bios.bin"
ROM = PROJ / "roms" / "minishcap_usa.gba"
START_KEYINPUT = 0x3F7
NONE_KEYINPUT = 0x3FF
IRQ_NOISE = 0x03007e00


def held(f):
    return f >= 200 and (f // 6) % 6 == 0


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
                ch = self.s.recv(1 << 20)
                if not ch:
                    raise RuntimeError("peer closed")
                self.b += ch
        finally:
            self.s.settimeout(None)
        line, _, self.b = self.b.partition(b"\n")
        return json.loads(line.decode())

    def read(self, cmd, base, size, chunk=4096):
        out = bytearray()
        for off in range(0, size, chunk):
            r = self.call(cmd=cmd, addr=base + off, len=min(chunk, size - off))
            if not r.get("ok"):
                raise RuntimeError(f"{cmd}@{base+off:#x}: {r}")
            out += bytes.fromhex(r["data"])
        return bytes(out)


def clusters(offs, gap=16):
    out = []
    for o in offs:
        if out and o - out[-1][1] <= gap:
            out[-1][1] = o
        else:
            out.append([o, o])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frame", type=int, default=682)
    ap.add_argument("--ewram", action="store_true")
    ap.add_argument("--timeout", type=float, default=60)
    args = ap.parse_args()
    env = dict(os.environ)
    procs = [
        subprocess.Popen([str(RECOMP_EXE), "--tcp", "19842"], cwd=str(PROJ), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
        subprocess.Popen([str(INTERP_EXE), "--bios", str(BIOS), "--rom", str(ROM),
                          "--tcp", "19844"], cwd=str(ROOT), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
    ]
    try:
        rec, intp = C(19842), C(19844)
        for f in range(1, args.frame + 1):
            mask = START_KEYINPUT if held(f) else NONE_KEYINPUT
            rec.call(cmd="set_keyinput", value=mask)
            intp.call(cmd="set_keyinput", value=mask)
            rec.call(timeout=args.timeout, cmd="step")
            intp.call(timeout=args.timeout, cmd="step")
        regions = [("read_iwram", 0x03000000, 0x8000, "IWRAM")]
        if args.ewram:
            regions.append(("read_ewram", 0x02000000, 0x40000, "EWRAM"))
        for cmd, base, size, tag in regions:
            rb = rec.read(cmd, base, size)
            ib = intp.read(cmd, base, size)
            offs = [i for i in range(min(len(rb), len(ib))) if rb[i] != ib[i]]
            real = [o for o in offs if not (base == 0x03000000 and base + o >= IRQ_NOISE)]
            print(f"== {tag} @f{args.frame}: {len(offs)} bytes differ "
                  f"({len(real)} excl IRQ-stack)", flush=True)
            for lo, hi in clusters(real)[:24]:
                print(f"   {base+lo:#08x}..{base+hi:#08x} ({hi-lo+1}B)  "
                      f"R={rb[lo:min(hi+1,lo+16)].hex()} I={ib[lo:min(hi+1,lo+16)].hex()}",
                      flush=True)
        for cl in (rec, intp):
            try: cl.call(cmd="quit")
            except Exception: pass
    finally:
        for p in procs:
            try: p.wait(timeout=8)
            except subprocess.TimeoutExpired: p.kill()


if __name__ == "__main__":
    sys.exit(main())
