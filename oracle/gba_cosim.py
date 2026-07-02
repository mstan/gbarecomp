#!/usr/bin/env python3
"""gba_cosim.py — GBA first-divergence co-simulation coordinator. See COSIM_ORACLE.md.

Launches two clean gba-cosim instances of the SAME binary — one recomp backend, one
force-interp (GBARECOMP_FORCE_INTERP=1) — advances BOTH to the same guest-cycle
checkpoints, and compares their full-state chain hashes. The first checkpoint whose
chains differ brackets the first divergence; the per-subsystem `sub` hashes + the
ring `window` say WHAT and WHERE.

The two instances are NOT interleaved on one timeline — they are two independent
deterministic runs, sampled at matched guest cycles (the ARM7 master clock, advanced
per-instruction identically by both backends via runtime_tick). Validity rests
ENTIRELY on determinism, which is why you MUST pass the gates first:

  # GATE 1 — determinism/hashing: two of the SAME backend must NEVER diverge.
  python gba_cosim.py --a recomp  --b recomp  --stride 65536 --max 4000000000
  python gba_cosim.py --a interp  --b interp  --stride 65536 --max 4000000000
  # GATE 3 — injected fault must halt at the right checkpoint + subsystem:
  python gba_cosim.py --a recomp --b recomp --inject-at 200000000 --inject ram:0:1000:1

  # THE RUN (only after gates pass):
  python gba_cosim.py --a recomp --b interp --stride 65536 --max 4000000000
"""
import socket, subprocess, os, sys, time, argparse

# Game-agnostic via env overrides (defaults = MinishCap gba-cosim build).
#   GBA_COSIM_EXE    path to the game's gba-cosim exe
#   GBA_COSIM_ROM    path to the ROM
#   GBA_COSIM_BIOS   path to gba_bios.bin
#   GBA_COSIM_CONFIG path to game.toml
_MC = r"F:\Projects\gbarecomp\MinishCapRecomp"
EXE    = os.environ.get("GBA_COSIM_EXE",    os.path.join(_MC, r"build-cosim\MinishCapRecomp.exe"))
ROM    = os.environ.get("GBA_COSIM_ROM",    os.path.join(_MC, "roms", "minishcap_usa.gba"))
BIOS   = os.environ.get("GBA_COSIM_BIOS",   r"F:\Projects\gbarecomp\gbarecomp-wt-cosim\bios\gba_bios.bin")
CONFIG = os.environ.get("GBA_COSIM_CONFIG", os.path.join(_MC, "game.toml"))
CWD    = os.environ.get("GBA_COSIM_CWD",    os.path.dirname(EXE))
LOGDIR = os.path.join(CWD, "cosim-logs")
# The GBA master clock is ~280896 cycles/frame (16.78 MHz / 59.7275 Hz).
CYCLES_PER_FRAME = 280896


def tail_file(path, max_bytes=8192):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - max_bytes), os.SEEK_SET)
            return f.read().decode(errors="replace")
    except Exception as e:
        return f"<could not read {path}: {e}>"


def launch(mode, port, stride):
    os.makedirs(LOGDIR, exist_ok=True)
    env = dict(os.environ)
    env["GBA_COSIM_PORT"] = str(port)
    env["GBA_COSIM_STRIDE"] = str(stride)
    # Both backends must execute every instruction identically: DISABLE Stage-2
    # idle-loop elision (recomp would jump the clock while interp steps the loop,
    # desyncing device state at a labeled checkpoint) and Stage-1 heal-recompile.
    env["GBARECOMP_IDLE_ELISION"] = "0"
    env["GBARECOMP_SELFHEAL_RECOMPILE"] = "0"
    # Pin RTC so a game that reads host time is deterministic (MinishCap has none;
    # harmless here, required for FRLG/RSE).
    env.setdefault("RECOMP_RTC_EPOCH", "1000000000")
    if mode == "interp":
        env["GBARECOMP_FORCE_INTERP"] = "1"
    else:
        env.pop("GBARECOMP_FORCE_INTERP", None)
    log_path = os.path.join(LOGDIR, f"cosim_{mode}_{port}.log")
    log_file = open(log_path, "wb")
    # --frames huge: free-run the headless frame loop; the cosim hook parks the
    # guest at every checkpoint until we grant budget, so it never actually runs
    # away. No --tcp (that is the debug server / wait loop, not free-run).
    argv = [EXE, "--frames", "100000000",
            "--rom", ROM, "--bios", BIOS, "--config", CONFIG, "--quiet"]
    p = subprocess.Popen(argv, cwd=CWD, env=env,
                         stdout=log_file, stderr=subprocess.STDOUT,
                         creationflags=0x00000200)  # CREATE_NEW_PROCESS_GROUP
    p._log_path = log_path
    p._log_file = log_file
    # Below-normal so a pair of oracle instances never starves the machine.
    try:
        import ctypes
        h = ctypes.windll.kernel32.OpenProcess(0x0400, False, p.pid)
        ctypes.windll.kernel32.SetPriorityClass(h, 0x00004000)  # BELOW_NORMAL
    except Exception:
        pass
    return p


def connect(port, timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.socket(); s.settimeout(5); s.connect(("127.0.0.1", port))
            return s
        except Exception:
            time.sleep(0.5)
    raise RuntimeError(f"cosim port {port} never came up")


def cmd(s, line, timeout=600):
    s.settimeout(timeout)
    s.sendall((line + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    return buf.decode(errors="replace").strip()


def kv(resp):
    """Parse a reply into {token: next_token} over ALL adjacent pairs, so 'chain'
    -> HEX is captured regardless of a leading bare status word (e.g. 'parked cp N
    ...'). A parity-sensitive parser that returned chain=None for BOTH sides would
    make every compare None==None == 'equal' — a silent blind spot Gate 3 catches."""
    t = resp.split()
    d = {}
    for i in range(len(t) - 1):
        d.setdefault(t[i], t[i + 1])
    return d


def wait_parked(sa, sb, timeout=600):
    t0 = time.time()
    last = ("", "")
    while time.time() - t0 < timeout:
        ra = kv(cmd(sa, "status", timeout=120))
        rb = kv(cmd(sb, "status", timeout=120))
        last = (ra, rb)
        if ra.get("parked") == "1" and rb.get("parked") == "1":
            return ra, rb
        time.sleep(0.05)
    raise RuntimeError(f"cosim did not park before timeout: A={last[0]} B={last[1]}")


def dump_window(s, n=16):
    s.settimeout(30); s.sendall(f"window {n}\n".encode()); buf = b""
    while b"\nend\n" not in buf and not buf.endswith(b"end"):
        d = s.recv(65536)
        if not d:
            break
        buf += d
        if buf.endswith(b"end\n") or buf.endswith(b"end"):
            break
    return buf.decode(errors="replace")


def report_child_failure(label, proc, cp, exc):
    print(f"[{label} reset] at local cp {cp}: poll={proc.poll()} error={exc}", flush=True)
    path = getattr(proc, "_log_path", "")
    if path:
        print(f"--- {label} log tail: {path} ---\n{tail_file(path)}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="recomp", choices=["recomp", "interp"])
    ap.add_argument("--b", default="interp", choices=["recomp", "interp"])
    ap.add_argument("--stride", type=int, default=65536)
    ap.add_argument("--max", type=int, default=4_000_000_000)  # guest cycles (~14k frames)
    ap.add_argument("--porta", type=int, default=19850)
    ap.add_argument("--portb", type=int, default=19851)
    ap.add_argument("--inject-at", type=int, default=0)
    ap.add_argument("--inject", default="")  # ram:<region>:<off>:<xor>  or  reg:<idx>:<xor>
    args = ap.parse_args()

    print(f"launch A={args.a}:{args.porta}  B={args.b}:{args.portb}  stride={args.stride}", flush=True)
    pa = launch(args.a, args.porta, args.stride)
    pb = launch(args.b, args.portb, args.stride)
    try:
        sa = connect(args.porta); sb = connect(args.portb)
        ia, ib = wait_parked(sa, sb)
        max_cp = args.max // args.stride
        print(f"both up; stepping up to {max_cp} checkpoints (stride {args.stride})", flush=True)
        print(f"initial park A cycle {ia.get('cycle')} cp {ia.get('cp')}  "
              f"B cycle {ib.get('cycle')} cp {ib.get('cp')}", flush=True)

        injected = not args.inject
        cp = 0
        while cp < max_cp:
            # Gate-3 injection into B just before the checkpoint that crosses inject_at.
            if not injected and (cp + 1) * args.stride >= args.inject_at:
                parts = args.inject.split(":")
                if parts[0] == "ram" and len(parts) == 4:
                    print(cmd(sb, f"inject ram {int(parts[1])} {int(parts[2])} {int(parts[3])}"), flush=True)
                elif parts[0] == "reg" and len(parts) == 3:
                    print(cmd(sb, f"inject reg {int(parts[1])} {int(parts[2])}"), flush=True)
                print(f"[inject] {args.inject} into B before cp {cp+1}", flush=True)
                injected = True

            try:
                ra = kv(cmd(sa, "step 1"))
            except Exception as e:
                report_child_failure("A", pa, cp, e); return
            try:
                rb = kv(cmd(sb, "step 1"))
            except Exception as e:
                report_child_failure("B", pb, cp, e); return
            if pa.poll() is not None or pb.poll() is not None:
                print(f"[exit] a={pa.poll()} b={pb.poll()} at cp {cp}", flush=True); break

            ca, cb = ra.get("chain"), rb.get("chain")
            if ca is None or cb is None:
                print(f"[FATAL] could not parse chain — tool is BLIND, aborting.\n"
                      f"  A: {ra}\n  B: {rb}", flush=True); return
            cyc_a, cyc_b = ra.get("cycle"), rb.get("cycle")
            if cyc_a != cyc_b:
                print(f"[WARN] cycle skew A={cyc_a} B={cyc_b} at cp {ra.get('cp')} — the two "
                      f"runs are NOT parking at the same cycle (harness nondeterminism or a "
                      f"per-instruction cycle-model mismatch between backends). Investigate "
                      f"before trusting a divergence.", flush=True)
            if ca != cb:
                fr = int(cyc_a) // CYCLES_PER_FRAME if cyc_a and cyc_a.isdigit() else -1
                print(f"\n*** FIRST DIVERGENCE at checkpoint cp={ra.get('cp')} "
                      f"cycle~{cyc_a} (frame~{fr}) ***", flush=True)
                print(f"  A chain={ca}  B chain={cb}", flush=True)
                print("  --- A subhash ---\n  " + cmd(sa, "sub"), flush=True)
                print("  --- B subhash ---\n  " + cmd(sb, "sub"), flush=True)
                print("  (the FIRST subsystem hash that differs is where it split)", flush=True)
                print("  --- A cpu ---\n  " + cmd(sa, "cpu"), flush=True)
                print("  --- B cpu ---\n  " + cmd(sb, "cpu"), flush=True)
                print("  --- A dev ---\n  " + cmd(sa, "dev"), flush=True)
                print("  --- B dev ---\n  " + cmd(sb, "dev"), flush=True)
                print("  --- A window ---\n" + dump_window(sa, 16), flush=True)
                print("  --- B window ---\n" + dump_window(sb, 16), flush=True)
                return
            cp += 1
            if cp % 256 == 0:
                fr = int(cyc_a) // CYCLES_PER_FRAME if cyc_a and cyc_a.isdigit() else -1
                print(f"  ok cp {cp} cycle {cyc_a} (frame~{fr}) chain {ca}", flush=True)

        print("no divergence within --max (or a process exited).", flush=True)
        if args.inject:
            print("NOTE: injection run — a clean 'no divergence' means the tool MISSED the "
                  "fault (gate-3 FAIL); it should have stopped at the injected checkpoint.", flush=True)
    finally:
        for p in (pa, pb):
            try:
                if p.poll() is None:
                    p.terminate()
            except Exception:
                pass
            try:
                p._log_file.close()
            except Exception:
                pass


if __name__ == "__main__":
    main()
