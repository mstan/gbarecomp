"""Opt-in real-cartridge netplay qualification; supply all images locally."""
import argparse
import os
import pathlib
import random
import re
import subprocess
import tempfile
from udp_relay import UdpRelay, reserve_routes

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("exe")
p.add_argument("rom")
p.add_argument("bios")
p.add_argument("save_type")
p.add_argument("state", help="matching paired warm snapshot, produced by the local probe")
p.add_argument("inputs", help="relative-frame controller script")
p.add_argument("--frames", type=int, default=120)
p.add_argument("--delay", action="store_true", help="use delay-sync instead of rollback")
p.add_argument("--natural", action="store_true", help="measure ordinary play without forced incorrect predictions")
p.add_argument("--input-delay", type=int, choices=range(2,21), metavar="2..20", help="override the match's six-frame WAN default; old qualification routes used 2")
p.add_argument("--min-fps", type=float, default=0, help="require each peer to sustain this forward rate (excludes startup/checkpoint transfer)")
p.add_argument("--timeout", type=int, default=300, help="simulation deadline in seconds (1..3600); barriers get another 125 seconds")
p.add_argument("--emerald-trade", action="store_true", help="require a reciprocal slot-zero Emerald trade persisted on both cartridges")
p.add_argument("--emerald-battle-ko", action="store_true", help="require a cable battle KO delivered to its owner's cartridge")
p.add_argument("--emerald-battle-result", action="store_true", help="require complementary cable battle win/loss records in both saves")
args = p.parse_args()
if not 1 <= args.timeout <= 3600:
    p.error("--timeout must be 1..3600 seconds")
for name in ("exe", "rom", "bios", "state", "inputs"):
    setattr(args, name, str(pathlib.Path(getattr(args, name)).resolve(strict=True)))

with tempfile.TemporaryDirectory(prefix="gba-commercial-net-") as tmp:
    root = pathlib.Path(tmp)
    port, sockets = reserve_routes()
    relay = UdpRelay(port,sockets)
    nonce = random.randrange(1, 2**31)
    peers, logs = [], []
    try:
        relay.start()
        for slot in range(2):
            env = {k: v for k, v in os.environ.items() if not k.startswith(
                ("GBA_LINK_PROBE_", "RNET_RB_", "GBA_RB_", "RNET_SIM_", "RBE_RB_"))}
            env.update(GBA_LINK_PROBE_STATE_IN=args.state, GBA_LINK_PROBE_INPUTS=args.inputs,
                GBA_LINK_PROBE_STATE_OUT=str(root / f"peer{slot}.state"),
                GBA_LINK_PROBE_SLICES="1", GBA_LINK_PROBE_NET_SEAT=str(slot),
                GBA_LINK_PROBE_NET_SESSION=str(nonce), GBA_LINK_PROBE_NET_BIND=f"127.0.0.1:{port+slot}",
                GBA_LINK_PROBE_NET_PEER=f"127.0.0.1:{port+2+slot}",
                GBA_LINK_PROBE_NET_ROLLBACK="0" if args.delay else "1",
                GBA_LINK_PROBE_NET_TIMEOUT_MS=str(args.timeout * 1000),
                GBA_RB_FORCE_MISPREDICT="7" if slot == 0 and not args.delay and not args.natural else "0")
            if args.input_delay is not None:
                env["GBA_LINK_PROBE_NET_DELAY"] = str(args.input_delay)
            log = open(root / f"peer{slot}.log", "w", encoding="utf-8")
            logs.append(log)
            peers.append(subprocess.Popen([args.exe,args.rom,args.bios,str(args.frames),args.save_type],
                env=env,stdout=log,stderr=log,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0))
        for peer in peers:
            assert peer.wait(timeout=args.timeout + 125) == 0, f"cartridge peer exited {peer.returncode}"
        reports = [(root / f"peer{i}.log").read_text(errors="replace") for i in range(2)]
        assert not relay.errors, relay.errors
        assert relay.delayed > 0 and relay.peak > 0, "latency injection was not exercised"
        for report in reports:
            assert "LINK SIMULATOR OVERFLOW" not in report, "latency simulator overflow invalidates qualification"
            match = re.search(r"network agreed tick=(\d+) hash=([0-9a-f]+) replay=(\d+) bytes=(\d+)",report)
            assert match and int(match[1]) == args.frames, "missing exact checkpoint agreement"
            if not args.delay and not args.natural:
                assert int(match[3]) > 0, "rollback was not exercised"
            print(match[0])
            performance=re.search(r"network performance forward_fps=([\d.]+) work_fps=([\d.]+) work_frames=(\d+)",report)
            if performance:
                print(performance[0])
            if args.min_fps:
                assert performance and float(performance[1]) >= args.min_fps, "forward frame rate below required minimum"
        left = (root / "peer0.state").read_bytes()
        right = (root / "peer1.state").read_bytes()
        assert left == right, "native cartridge session bytes diverged"
        if args.emerald_trade:
            from emerald_trade_check import check_trade
            check_trade(args.state, root / "peer0.state")
        if args.emerald_battle_ko:
            from emerald_battle_check import check_battle
            check_battle(args.state, root / "peer0.state")
        if args.emerald_battle_result:
            from emerald_battle_check import check_result
            check_result(args.state, root / "peer0.state")
        print(f"{'delay-sync' if args.delay else 'rollback'} native cartridge: {len(left)} identical bytes, 40 ms latency / 10 ms jitter per direction")
    except Exception:
        for log in logs:
            log.flush()
        for path in sorted(root.glob("*.log")):
            print(path.name,path.read_text(errors="replace")[-20000:])
        raise
    finally:
        relay.close()
        for peer in peers:
            if peer.poll() is None:
                peer.terminate()
                peer.wait(timeout=5)
        for log in logs:
            log.close()
