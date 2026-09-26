"""Two separate native processes, replicated link session, real UDP + netsim."""
import os
import pathlib
import random
import socket
import subprocess
import sys
import tempfile

exe = str(pathlib.Path(sys.argv[1]).resolve())
rollback = int(sys.argv[2])
mode = sys.argv[3] if len(sys.argv) > 3 else "ordinary"
with tempfile.TemporaryDirectory(prefix="gba-link-net-") as tmp:
    root = pathlib.Path(tmp)
    # Reserve/check a pair, then release immediately before the peers bind.
    for _ in range(100):
        port = random.randrange(20000, 55000)
        a, b = socket.socket(type=socket.SOCK_DGRAM), socket.socket(type=socket.SOCK_DGRAM)
        try:
            a.bind(("127.0.0.1", port)); b.bind(("127.0.0.1", port + 1))
            break
        except OSError:
            continue
        finally:
            a.close(); b.close()
    else:
        raise RuntimeError("No free loopback port pair")
    nonce = random.randrange(1, 2**31)
    peers, logs = [], []
    try:
        for slot in range(2):
            env = {k: v for k, v in os.environ.items() if not k.startswith(("RNET_RB_", "GBA_RB_", "RNET_SIM_", "RBE_RB_", "GBA_TEST_"))}
            env.update(RNET_SIM_LATENCY_MS="40", RNET_SIM_JITTER_MS="10", RNET_SIM_SEED=str(17 + slot))
            if mode == "loss":
                env["RNET_SIM_LOSS_PCT"] = "2"
            if mode == "mismatch" and slot == 1:
                env["GBA_TEST_BOOT_ID"] = "incompatible-runtime-build"
            # Force corrections as well as naturally late rows: verify restored
            # state matters, not just that idle peers reach the same final PC.
            env["GBA_RB_FORCE_MISPREDICT"] = "7" if rollback and slot == 0 else "0"
            log = open(root / f"peer{slot}.log", "w", encoding="utf-8")
            logs.append(log)
            peers.append(subprocess.Popen([exe, str(slot), str(port), str(nonce), str(rollback), str(root / f"peer{slot}.txt")],
                stdout=log, stderr=log, env=env,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0))
        if mode == "mismatch":
            assert peers[0].wait(timeout=15) == 11, "host accepted incompatible startup"
            assert "program identity mismatch" in (root / "peer0.log").read_text(errors="replace")
            print("startup refused a different runtime/build identity before any guest frame")
            sys.exit(0)
        for peer in peers:
            if peer.wait(timeout=110):
                raise AssertionError(f"peer exited {peer.returncode}")
        reports = [(root / f"peer{i}.txt").read_text().splitlines() for i in range(2)]
        timelines = [dict(line.split() for line in r[1:]) for r in reports]
        common = timelines[0].keys() & timelines[1].keys()
        assert len(common) >= 60, f"insufficient confirmed history: {len(common)}"
        assert all(timelines[0][t] == timelines[1][t] for t in common), "confirmed simulation timelines diverged"
        if rollback:
            assert all(int(r[0].split()[3]) > 0 for r in reports), "rollback was not exercised by both peers"
        print(f"{'rollback' if rollback else 'delay-sync'}: {len(common)} matching confirmed ticks, 40 ms latency / 10 ms jitter, {'2' if mode == 'loss' else '0'}% loss")
        for r in reports:
            print(r[0])
    except Exception:
        for log in logs:
            log.flush()
        for path in sorted(root.glob("*.log")):
            print(path.name, path.read_text(errors="replace")[-18000:])
        raise
    finally:
        for peer in peers:
            if peer.poll() is None:
                peer.terminate(); peer.wait(timeout=5)
        for log in logs:
            log.close()
