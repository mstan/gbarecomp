"""Two separate native processes, replicated link session, real UDP + netsim."""
import os
import pathlib
import random
import subprocess
import sys
import tempfile
import time
from udp_relay import UdpRelay, reserve_routes

exe = str(pathlib.Path(sys.argv[1]).resolve())
rollback = int(sys.argv[2])
mode = sys.argv[3] if len(sys.argv) > 3 else "ordinary"
with tempfile.TemporaryDirectory(prefix="gba-link-net-") as tmp:
    root = pathlib.Path(tmp)
    port, proxy_sockets = reserve_routes()
    nonce = random.randrange(1, 2**31)
    peers, logs = [], []
    relay = UdpRelay(port,proxy_sockets,mode,root / "outage")
    try:
        relay.start()
        for slot in range(2):
            env = {k: v for k, v in os.environ.items() if not k.startswith(("RNET_RB_", "GBA_RB_", "RNET_SIM_", "RBE_RB_", "GBA_TEST_"))}
            env["GBA_TEST_PEER_PORT"] = str(port+2+slot)
            if mode == "mismatch" and slot == 1:
                env["GBA_TEST_BOOT_ID"] = "incompatible-runtime-build"
            if mode == "checkpoint_mismatch" and slot == 1:
                env["GBA_TEST_CHECKPOINT_CORRUPT"] = "1"
            if mode == "restart":
                env["GBA_TEST_RESTART"] = "1"
            if mode == "outage":
                env["GBA_TEST_OUTAGE_TRIGGER"] = str(root / "outage")
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
        if mode == "checkpoint_mismatch":
            assert not rollback, "checkpoint corruption scenario requires the exact delay-sync tip"
            assert peers[1].wait(timeout=80) == 14, "guest accepted a different paired checkpoint"
            assert "checkpoint state digest mismatch" in (root / "peer1.log").read_text(errors="replace")
            assert not list(root.glob("*.paired")), "a mismatched checkpoint was exported"
            print("checkpoint barrier rejected a different guest state without exporting either archive")
            sys.exit(0)
        for peer in peers:
            if peer.wait(timeout=110):
                raise AssertionError(f"peer exited {peer.returncode}")
        reports = [(root / f"peer{i}.txt").read_text().splitlines() for i in range(2)]
        timelines = [dict(line.split() for line in r[1:]) for r in reports]
        common = timelines[0].keys() & timelines[1].keys()
        assert len(common) >= 60, f"insufficient confirmed history: {len(common)}"
        assert all(timelines[0][t] == timelines[1][t] for t in common), "confirmed simulation timelines diverged"
        assert (root / "peer0.txt.paired").read_bytes() == (root / "peer1.txt.paired").read_bytes(), "agreed paired archives differ"
        if mode == "restart":
            assert (root / "peer0.txt.restart").read_bytes() == (root / "peer1.txt.restart").read_bytes(), "warm restart timelines differ"
            print("restored the agreed archive on a fresh connection and matched 30 further input ticks")
        if rollback:
            assert all(int(r[0].split()[3]) > 0 for r in reports), "rollback was not exercised by both peers"
        if mode == "outage":
            assert not relay.errors, relay.errors
            assert relay.outage_started is not None and relay.dropped > 0, "outage never interrupted gameplay"
            assert time.monotonic()-relay.outage_started >= 6, "peers finished during the outage"
            assert all("reconnecting 1 recovered 1" in r[0] for r in reports), "connection grace was not exercised"
            print(f"recovered after a 6-second complete UDP outage ({relay.dropped} datagrams dropped)")
        if mode == "startup_loss":
            assert relay.upload_acks_dropped > 0 and relay.ready_acks_dropped == 3, "startup loss was not exercised"
            print("startup survived lost upload ACKs and lost ready replies")
        assert not relay.errors, relay.errors
        assert relay.delayed > 0 and relay.peak > 0, "latency injection was not exercised"
        if mode == "loss":
            assert relay.dropped > 0, "packet loss was not exercised"
        print(f"{'rollback' if rollback else 'delay-sync'}: {len(common)} matching confirmed ticks, 40 ms latency / 10 ms jitter, {'2' if mode == 'loss' else '0'}% loss")
        for r in reports:
            print(r[0])
    except Exception:
        if relay.errors:
            print("relay errors:", relay.errors)
        for log in logs:
            log.flush()
        for path in sorted(root.glob("*.log")):
            print(path.name, path.read_text(errors="replace")[-18000:])
        raise
    finally:
        relay.close()
        for peer in peers:
            if peer.poll() is None:
                peer.terminate(); peer.wait(timeout=5)
        for log in logs:
            log.close()
