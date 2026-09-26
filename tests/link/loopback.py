"""Two separate native processes, replicated link session, real UDP + netsim."""
import os
import pathlib
import random
import socket
import subprocess
import sys
import tempfile
import select
import threading
import time
import heapq
import struct

exe = str(pathlib.Path(sys.argv[1]).resolve())
rollback = int(sys.argv[2])
mode = sys.argv[3] if len(sys.argv) > 3 else "ordinary"
with tempfile.TemporaryDirectory(prefix="gba-link-net-") as tmp:
    root = pathlib.Path(tmp)
    # External simulation covers large boot transfers as well as input traffic.
    # recomp-net's internal 256-packet hold queue can overflow during STATE
    # bursts and deliver undelayed packets, invalidating a latency result.
    proxy_sockets = []
    for _ in range(100):
        port = random.randrange(20000, 55000)
        sockets = [socket.socket(type=socket.SOCK_DGRAM) for _ in range(4)]
        try:
            for i, sock in enumerate(sockets):
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4*1024*1024)
                sock.bind(("127.0.0.1", port + i))
                sock.setblocking(False)
            proxy_sockets = sockets[2:]
            break
        except OSError:
            for sock in sockets:
                sock.close()
            continue
        finally:
            for sock in sockets[:2]:
                sock.close()
    else:
        raise RuntimeError("No free loopback port pair")
    nonce = random.randrange(1, 2**31)
    peers, logs = [], []
    relay_stop = threading.Event()
    outage_started = None
    dropped = 0
    delayed = 0
    peak = 0
    upload_acks_dropped = 0
    ready_acks_dropped = 0
    relay_error = []
    def relay():
        global outage_started, dropped, delayed, peak, upload_acks_dropped, ready_acks_dropped
        pending = []
        rng = [random.Random(17), random.Random(18)]
        try:
            while not relay_stop.is_set():
                if outage_started is None and (root / "outage").exists():
                    outage_started = time.monotonic()
                now = time.monotonic()
                while pending and pending[0][0] <= now:
                    _, _, sender, packet = heapq.heappop(pending)
                    if outage_started is not None and now - outage_started < 6:
                        dropped += 1
                    else:
                        proxy_sockets[1-sender].sendto(packet, ("127.0.0.1", port + 1-sender))
                timeout = min(0.005, max(0, pending[0][0]-now)) if pending else 0.005
                ready, _, _ = select.select(proxy_sockets, [], [], timeout)
                for sock in ready:
                    try:
                        packet, _ = sock.recvfrom(65535)
                    except (ConnectionResetError, BlockingIOError):
                        # Windows reports an early UDP send to a not-yet-bound
                        # peer as an ICMP error on the next recv. HELLO retries.
                        continue
                    if outage_started is not None and time.monotonic() - outage_started < 6:
                        dropped += 1
                        continue
                    sender = proxy_sockets.index(sock)
                    if mode == "startup_loss" and len(packet) >= 24:
                        # Pinned recomp-net wire: type u16 at 4, xfer id u32
                        # at 14. Lose every upload ACK: completed BOOT must be
                        # accepted as the host's receipt. Then lose ready
                        # replies so the coordination barrier must retransmit.
                        kind = struct.unpack_from("<H", packet, 4)[0]
                        transfer = struct.unpack_from("<I", packet, 14)[0]
                        if sender == 0 and kind == 10 and transfer == 0x40000001:
                            upload_acks_dropped += 1
                            continue
                        if sender == 1 and kind == 12 and ready_acks_dropped < 3:
                            ready_acks_dropped += 1
                            continue
                    if mode == "loss" and rng[sender].randrange(100) < 2:
                        dropped += 1
                        continue
                    if len(pending) >= 8192:
                        raise RuntimeError("UDP simulation queue overflow; latency result invalid")
                    delayed += 1
                    heapq.heappush(pending, (time.monotonic()+rng[sender].uniform(0.030,0.050),
                                            delayed, sender, packet))
                    peak = max(peak,len(pending))
        except Exception as exc:
            relay_error.append(exc)
    relay_thread = threading.Thread(target=relay, daemon=True) if proxy_sockets else None
    try:
        if relay_thread:
            relay_thread.start()
        for slot in range(2):
            env = {k: v for k, v in os.environ.items() if not k.startswith(("RNET_RB_", "GBA_RB_", "RNET_SIM_", "RBE_RB_", "GBA_TEST_"))}
            env["GBA_TEST_PEER_PORT"] = str(port+2+slot)
            if mode == "mismatch" and slot == 1:
                env["GBA_TEST_BOOT_ID"] = "incompatible-runtime-build"
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
        if mode == "outage":
            assert not relay_error, relay_error
            assert outage_started is not None and dropped > 0, "outage never interrupted gameplay"
            assert time.monotonic()-outage_started >= 6, "peers finished during the outage"
            assert all("reconnecting 1 recovered 1" in r[0] for r in reports), "connection grace was not exercised"
            print(f"recovered after a 6-second complete UDP outage ({dropped} datagrams dropped)")
        if mode == "startup_loss":
            assert upload_acks_dropped > 0 and ready_acks_dropped == 3, "startup loss was not exercised"
            print("startup survived lost upload ACKs and lost ready replies")
        assert not relay_error, relay_error
        assert delayed > 0 and peak > 0, "latency injection was not exercised"
        if mode == "loss":
            assert dropped > 0, "packet loss was not exercised"
        print(f"{'rollback' if rollback else 'delay-sync'}: {len(common)} matching confirmed ticks, 40 ms latency / 10 ms jitter, {'2' if mode == 'loss' else '0'}% loss")
        for r in reports:
            print(r[0])
    except Exception:
        if relay_error:
            print("relay errors:", relay_error)
        for log in logs:
            log.flush()
        for path in sorted(root.glob("*.log")):
            print(path.name, path.read_text(errors="replace")[-18000:])
        raise
    finally:
        relay_stop.set()
        if relay_thread:
            relay_thread.join(timeout=5)
        for sock in proxy_sockets:
            sock.close()
        for peer in peers:
            if peer.poll() is None:
                peer.terminate(); peer.wait(timeout=5)
        for log in logs:
            log.close()
