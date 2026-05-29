#!/usr/bin/env python3
"""trace_at_write.py — query the always-on runtime trace ring at the exact
moment a watched memory byte is written, for a given (state, held key).

This is the structured "which instruction wrote this byte, and via what
call/branch chain?" probe. It does NOT arm-then-run: the trace ring records
continuously from process start; the frame-gated mem-write watchpoint
(GBARECOMP_ABORT_ON_MEM_WRITE_ADDR / _MIN_FRAME) fires synchronously the
instant the watched write happens at/after the gate frame, dumping the
trailing ring (GBARECOMP_TRACE_DUMP_DEPTH events) to stderr. We just drive
the engine to that point and capture the dump.

Use it to localize a divergent writer found by diff_iwram.py: point --addr
at the diverging byte, set --min-frame to one below the divergence frame,
hold the same key, load the same state.

Usage:
    python oracle/trace_at_write.py --addr 0x03004470 --min-frame 39 \
        --hold up --frames 45 --depth 4096
"""
from __future__ import annotations
import argparse, json, os, pathlib, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJ = ROOT.parent / "MinishCapRecomp"
RECOMP_EXE = PROJ / "build" / "MinishCapRecomp.exe"
DEF_STATE = PROJ / "roms" / "minishcap_usa.state3"
OUT = ROOT / "oracle" / "trace_out"

KEYMASK = {"up": 0x3FF & ~0x40, "down": 0x3FF & ~0x80,
           "left": 0x3FF & ~0x20, "right": 0x3FF & ~0x10,
           "a": 0x3FF & ~0x01, "none": 0x3FF}


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", required=True,
                    help="watched write address, e.g. 0x03004470")
    ap.add_argument("--min-frame", type=int, default=39,
                    help="suppress the abort until this many VBlank-starts")
    ap.add_argument("--value", default=None,
                    help="only fire when the written value matches (e.g. 0x0b)")
    ap.add_argument("--hold", default="up")
    ap.add_argument("--state", default=str(DEF_STATE))
    ap.add_argument("--frames", type=int, default=50)
    ap.add_argument("--depth", type=int, default=4096)
    ap.add_argument("--port", type=int, default=19842)
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()

    addr = int(args.addr, 0)
    mask = KEYMASK.get(args.hold.lower(), 0x3FF)
    OUT.mkdir(parents=True, exist_ok=True)
    errpath = OUT / "recomp_stderr.txt"

    env = dict(os.environ)
    env["GBARECOMP_ABORT_ON_MEM_WRITE_ADDR"] = hex(addr)
    env["GBARECOMP_ABORT_ON_MEM_WRITE_MIN_FRAME"] = str(args.min_frame)
    env["GBARECOMP_TRACE_DUMP_DEPTH"] = str(args.depth)
    if args.value is not None:
        env["GBARECOMP_ABORT_ON_MEM_WRITE_VALUE"] = hex(int(args.value, 0))

    print(f"==> spawn recomp watchpoint addr={addr:#010x} "
          f"min_frame={args.min_frame} depth={args.depth}", flush=True)
    errf = open(errpath, "wb")
    proc = subprocess.Popen([str(RECOMP_EXE), "--tcp", str(args.port)],
                            cwd=str(PROJ), stdout=subprocess.DEVNULL, stderr=errf,
                            env=env)
    aborted_at = None
    try:
        cl = C(args.port)
        r = cl.call(cmd="savestate_load", path=args.state)
        if not r.get("ok"):
            print(f"load failed: {r}"); return 1
        cl.call(cmd="set_keyinput", value=mask)
        print(f"==> loaded state, holding '{args.hold}', stepping…", flush=True)
        for f in range(1, args.frames + 1):
            try:
                cl.call(timeout=args.timeout, cmd="step")
            except (socket.timeout, RuntimeError, OSError) as e:
                aborted_at = f
                print(f"==> step {f}: connection dropped ({type(e).__name__}) "
                      f"— watchpoint likely fired", flush=True)
                break
        else:
            print(f"==> ran {args.frames} steps with NO abort — "
                  f"watchpoint never fired (wrong addr/frame?)", flush=True)
            try: cl.call(cmd="quit")
            except Exception: pass
    finally:
        try: proc.wait(timeout=6)
        except subprocess.TimeoutExpired: proc.kill()
        errf.close()

    data = errpath.read_bytes().decode("utf-8", "replace")
    abort_lines = [ln for ln in data.splitlines()
                   if "mem-write-addr abort" in ln]
    print(f"==> stderr captured: {errpath} ({len(data)} bytes, "
          f"{data.count(chr(10))} lines)", flush=True)
    if abort_lines:
        print("==> ABORT: " + abort_lines[-1], flush=True)
    if aborted_at:
        print(f"==> fired during step {aborted_at}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
