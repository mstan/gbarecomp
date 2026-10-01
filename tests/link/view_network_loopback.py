"""Qualify a game's real view callbacks over four latency-injected peers.

Assets and a warm linked-gameplay snapshot are supplied locally. Each peer
renders its own machine at the requested width; canonical snapshots must agree.
"""
import argparse
import os
import pathlib
import random
import re
import subprocess
from udp_relay import UdpRelay, reserve_routes

p = argparse.ArgumentParser(description=__doc__)
for name in ("exe", "rom", "bios", "state", "output"):
    p.add_argument(name)
p.add_argument("--inputs")
p.add_argument("--frames", type=int, default=300)
p.add_argument("--widths", default="240,284,373,569")
p.add_argument("--rollback", action="store_true")
p.add_argument("--min-fps", type=float, default=0)
p.add_argument("--input-delay", type=int, choices=range(2,21), default=6)
args = p.parse_args()
for name in ("exe", "rom", "bios", "state", "inputs"):
    if getattr(args, name):
        setattr(args, name, str(pathlib.Path(getattr(args, name)).resolve(strict=True)))
widths = [int(n) for n in args.widths.split(",")]
assert 2 <= len(widths) <= 4 and all(240 <= w <= 896 for w in widths)
root = pathlib.Path(args.output).resolve()
root.mkdir(parents=True, exist_ok=False)
players = len(widths)
port, sockets = reserve_routes(players)
relay = UdpRelay(port, sockets)
nonce = random.randrange(1, 2**31)
peers, logs = [], []
try:
    relay.start()
    for seat, width in enumerate(widths):
        env = {k: v for k, v in os.environ.items() if not k.startswith(
            ("GBA_LINK_PROBE_", "GBA_VIEW_PROBE_", "RNET_RB_", "GBA_RB_", "RNET_SIM_", "RBE_RB_"))}
        env.update(GBA_LINK_PROBE_NET_SEAT=str(seat), GBA_LINK_PROBE_NET_SESSION=str(nonce),
            GBA_LINK_PROBE_NET_BIND=f"127.0.0.1:{port+seat}",
            GBA_LINK_PROBE_NET_PEER=f"127.0.0.1:{port+players+seat}",
            GBA_LINK_PROBE_NET_ROLLBACK="1" if args.rollback else "0",
            GBA_LINK_PROBE_NET_DELAY=str(args.input_delay),
            GBA_VIEW_PROBE_VIEW=str(width), GBARECOMP_STRICT_STATIC="1",
            GBARECOMP_SELFHEAL_RECOMPILE="0")
        command = [args.exe, args.rom, args.bios, args.state, str(root / f"seat{seat}"),
                   str(players), str(args.frames)]
        if args.inputs:
            command.append(args.inputs)
        log = open(root / f"seat{seat}.log", "w", encoding="utf-8")
        logs.append(log)
        peers.append(subprocess.Popen(command, env=env, stdout=log, stderr=log,
            creationflags=(subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == "nt" else 0))
    for peer in peers:
        assert peer.wait(timeout=420) == 0, f"view peer exited {peer.returncode}"
    for log in logs:
        log.flush()
    expected = (root / "seat0.state").read_bytes()
    assert all((root / f"seat{i}.state").read_bytes() == expected for i in range(players))
    for seat, width in enumerate(widths):
        report = (root / f"seat{seat}.log").read_text(errors="replace")
        assert f"network agreed tick={args.frames}" in report
        performance = re.search(r"network performance forward_fps=([\d.]+) work_fps=([\d.]+)", report)
        assert performance, "missing performance result"
        print(f"seat={seat} width={width} {performance[0]}")
        if args.min_fps:
            assert float(performance[1]) >= args.min_fps, "forward frame rate below minimum"
    assert not relay.errors and relay.delayed > 0
    print(f"PASS: {players} peers, {len(expected)} identical canonical bytes, 40 ms latency / 10 ms jitter per direction")
except Exception:
    for log in logs:
        log.flush()
    for path in sorted(root.glob("*.log")):
        print(path.name, path.read_text(errors="replace")[-6000:])
    raise
finally:
    relay.close()
    for peer in peers:
        if peer.poll() is None:
            peer.terminate()
            peer.wait(timeout=5)
    for log in logs:
        log.close()
