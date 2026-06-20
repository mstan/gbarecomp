#!/usr/bin/env python3
"""diff_anim.py — MC-HP-002 root cause: compare the recompiled runtime
against the INTERPRETER oracle from the same savestate, holding the same
input, watching the spinning entity's animation index (entity+0x12).

The recomp force-animates gEntities slot 0x030018D0 with animIdx 0 ->
animTable[0]=NULL -> garbage frame ptr -> UpdateAnimationVariableFrames
spins (hang ~frame 46). The interpreter runs the SAME ARM code from the
same restored state. If the interpreter sets a NONZERO animIdx where the
recomp leaves 0, the divergence is a recompiler bug (localizable). If the
interpreter ALSO reaches index 0 / spins, the divergence is elsewhere
(shared state) and needs the mGBA hardware oracle.

Both processes auto-spawn. Recomp on 19842, interpreter (bios_smoke) on
19844. Loads the savestate into both, holds Up, steps frame-by-frame with
a per-step timeout, and prints entity+0x10..0x13 from each side each frame,
flagging the first divergence.

Usage:
    python oracle/diff_anim.py [--state <path>] [--frames 60] [--hold up]
"""
from __future__ import annotations
import argparse, json, pathlib, socket, subprocess, sys, time
from typing import Optional

ROOT = pathlib.Path(__file__).resolve().parent.parent          # gbarecomp/
import recomp_paths as _rp
PROJ = _rp.game_dir(ROOT)
RECOMP_EXE = _rp.recomp_exe(ROOT)
INTERP_EXE = ROOT / "build" / "bios_smoke.exe"
BIOS = ROOT / "bios" / "gba_bios.bin"
ROM = PROJ / "roms" / "minishcap_usa.gba"
DEF_STATE = PROJ / "roms" / "minishcap_usa.state3"

ENTITY = 0x030018D0
ANIM_OFF = 0x12

# GBA KEYINPUT active-low: A0 B1 Sel2 Start3 R4 L5 Up6 Dn7 R8 L9
KEYMASK = {"up": 0x3FF & ~0x40, "down": 0x3FF & ~0x80,
           "left": 0x3FF & ~0x20, "right": 0x3FF & ~0x10, "none": 0x3FF}


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

    def read_bytes(self, base, n):
        r = self.call(cmd="read_iwram", addr=base, len=n)
        if not r.get("ok"):
            raise RuntimeError(f"read_iwram@{base:#x}: {r}")
        return bytes.fromhex(r["data"])


def anim_fields(b):
    # b is 0x20 bytes from ENTITY base
    u16 = lambda o: b[o] | (b[o + 1] << 8)
    return {"animIdx@12": u16(0x12), "kind@8": b[8], "id@9": b[9],
            "f10": b[0x10], "f11": b[0x11]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default=str(DEF_STATE))
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--hold", default="up")
    ap.add_argument("--timeout", type=float, default=12.0)
    ap.add_argument("--no-spawn", action="store_true")
    args = ap.parse_args()
    mask = KEYMASK.get(args.hold.lower(), 0x3FF)

    procs = []
    try:
        if not args.no_spawn:
            for exe in (RECOMP_EXE, INTERP_EXE):
                if not exe.exists():
                    print(f"missing {exe}", file=sys.stderr); return 1
            procs.append(subprocess.Popen(
                [str(RECOMP_EXE), "--tcp", "19842"], cwd=str(PROJ),
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            procs.append(subprocess.Popen(
                [str(INTERP_EXE), "--bios", str(BIOS), "--rom", str(ROM),
                 "--tcp", "19844"], cwd=str(ROOT),
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

        rec = JsonClient("127.0.0.1", 19842)
        intp = JsonClient("127.0.0.1", 19844)
        print("==> connected: recomp=19842 interp=19844", flush=True)

        for who, cl in (("recomp", rec), ("interp", intp)):
            r = cl.call(cmd="savestate_load", path=args.state)
            print(f"==> {who} savestate_load: {r}", flush=True)
            if not r.get("ok"):
                return 1
            cl.call(cmd="set_keyinput", value=mask)

        # Sanity: entity region identical right after load?
        rb = rec.read_bytes(ENTITY, 0x20)
        ib = intp.read_bytes(ENTITY, 0x20)
        print(f"   @load recomp {anim_fields(rb)}")
        print(f"   @load interp {anim_fields(ib)}")
        print(f"   @load region {'IDENTICAL' if rb == ib else 'DIFFERS'}")

        PRIO = 0x03003DC0  # gPriorityHandler; EntityDisabled compares it vs
                           # entity[0x11]&0xF to pause low-priority objects.
        rec_dead = intp_dead = False
        for f in range(1, args.frames + 1):
            rb = rec.read_bytes(ENTITY, 0x20) if not rec_dead else None
            ib = intp.read_bytes(ENTITY, 0x20) if not intp_dead else None
            rp = rec.read_bytes(PRIO, 4).hex() if not rec_dead else "(spun)"
            ip = intp.read_bytes(PRIO, 4).hex() if not intp_dead else "(spun)"
            ra = anim_fields(rb)["animIdx@12"] if rb else "(spun)"
            ia = anim_fields(ib)["animIdx@12"] if ib else "(spun)"
            flag = ""
            if rb and ib and ra != ia:
                flag = "  <-- animIdx DIVERGE"
            if rp != ip and "(spun)" not in (rp, ip):
                flag += "  <-- gPriorityHandler DIVERGE"
            print(f"f{f:3d}: recomp[anim={ra!s:>5} prio={rp}]  "
                  f"interp[anim={ia!s:>5} prio={ip}]{flag}", flush=True)
            if flag:
                print(f"   recomp {anim_fields(rb)}")
                print(f"   interp {anim_fields(ib)}")
            # step both (timeout = spin detection)
            if not rec_dead:
                try:
                    rec.call(timeout=args.timeout, cmd="step")
                except socket.timeout:
                    print(f"   recomp SPUN at frame {f} (step timeout)", flush=True)
                    rec_dead = True
            if not intp_dead:
                try:
                    intp.call(timeout=args.timeout, cmd="step")
                except socket.timeout:
                    print(f"   interp SPUN at frame {f} (step timeout)", flush=True)
                    intp_dead = True
            if rec_dead and intp_dead:
                print("   both spun — index-0 reached on BOTH; divergence is "
                      "shared state, not recomp codegen. Need mGBA oracle.",
                      flush=True)
                break
            if rec_dead and not intp_dead:
                print("   recomp spun but interp did NOT — RECOMP BUG. "
                      "Compare execution into the +0x12 setter.", flush=True)
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
