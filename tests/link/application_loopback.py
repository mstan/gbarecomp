"""Opt-in smoke of the actual game executable's SDL/netplay/asset entry path.

Supply local licensed images and two save paths. SDL dummy drivers keep this
noninteractive; this does not qualify monitor pacing or audible output.
"""
import argparse
import hashlib
import os
import pathlib
import random
import subprocess
import tempfile
from udp_relay import UdpRelay, reserve_routes

p=argparse.ArgumentParser(description=__doc__)
p.add_argument("exe")
p.add_argument("rom")
p.add_argument("bios")
p.add_argument("save0")
p.add_argument("save1")
p.add_argument("--frames",type=int,default=120)
p.add_argument("--resume-frames",type=int,default=60)
p.add_argument("--delay",action="store_true")
p.add_argument("--lan-contract",action="store_true",
               help="shared launcher's passive host and ephemeral guest, without the latency relay")
p.add_argument("--save-type",choices=["flash1m","flash512","eeprom"],default="flash1m")
p.add_argument("--save-size",type=int,default=131072)
args=p.parse_args()
for name in ("exe","rom","bios","save0","save1"):
    setattr(args,name,str(pathlib.Path(getattr(args,name)).resolve(strict=True)))
saves=[pathlib.Path(args.save0),pathlib.Path(args.save1)]
before=[hashlib.sha256(s.read_bytes()).digest() for s in saves]
with tempfile.TemporaryDirectory(prefix="gba-application-net-") as tmp:
    root=pathlib.Path(tmp)
    port,sockets=reserve_routes()
    relay=None if args.lan_contract else UdpRelay(port,sockets)
    if not relay:
        for sock in sockets: sock.close()
    peers=[]; logs=[]
    try:
        if relay: relay.start()
        def round_trip(label,frames,resume=None):
            nonce=random.randrange(1,2**31)
            current=[]
            paths=[root/f"{label}-peer{seat}.paired" for seat in range(2)]
            for seat in range(2):
                # A temporary config contains hardware size only. Original saves
                # are opened read-only; only new paired archives may be written.
                config=root/f"peer{seat}.toml"
                config.write_text(f'[save]\ntype = "{args.save_type}"\nsize = {args.save_size}\n')
                bind=f"127.0.0.1:{port+seat}"
                peer=f"127.0.0.1:{port+2+seat}"
                if args.lan_contract:
                    bind=f"0.0.0.0:{0 if seat else port}"
                    peer=f"127.0.0.1:{port}" if seat else ""
                argv=[args.exe,"--config",str(config),"--rom",args.rom,"--bios",args.bios,
                      "--save",str(saves[seat]),"--frames",str(frames),"--scale","1",
                      "--netplay-bind",bind,"--netplay-peer",peer,
                      "--netplay-seat",str(seat),"--netplay-session",str(nonce),
                      "--netplay-checkpoint",str(paths[seat])]
                if resume: argv.extend(["--netplay-resume",str(resume[seat])])
                if args.delay: argv.append("--netplay-delay-sync")
                env={k:v for k,v in os.environ.items() if not k.startswith(("GBARECOMP_","GBA_RB_","RNET_RB_","RBE_RB_"))}
                env.update(SDL_VIDEODRIVER="dummy",SDL_AUDIODRIVER="dummy",SDL_RENDER_DRIVER="software")
                log=open(root/f"{label}-peer{seat}.log","w",encoding="utf-8"); logs.append(log)
                current.append(subprocess.Popen(argv,cwd=root,env=env,stdout=log,stderr=log,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name=="nt" else 0))
                peers.append(current[-1])
            for peer in current: assert peer.wait(timeout=150)==0, f"application exited {peer.returncode}"
            for log in logs: log.flush()
            for seat in range(2):
                report=(root/f"{label}-peer{seat}.log").read_text(errors="replace")
                assert f"netplay agreed tick={frames}" in report,report[-8000:]
            assert paths[0].read_bytes()==paths[1].read_bytes(),"application paired archives differ"
            return paths

        archives=round_trip("cold",args.frames)
        left=archives[0].read_bytes()
        if args.resume_frames:
            restarted=round_trip("resume",args.resume_frames,archives)
            assert restarted[0].read_bytes()!=left,"resumed application did not advance"
            assert archives[0].read_bytes()==left,"resume overwrote its input archive"
            print(f"fresh game processes resumed the pair and agreed {args.resume_frames} further ticks")
        if relay:
            assert not relay.errors,relay.errors
            assert relay.delayed>0
        assert before==[hashlib.sha256(s.read_bytes()).digest() for s in saves],"original save changed"
        print(f"actual game application {'delay-sync' if args.delay else 'rollback'}: {args.frames} frames, "
              f"{len(left)} identical paired-archive bytes; original saves unchanged; SDL dummy video/audio; "
              f"{'passive LAN host/ephemeral guest' if args.lan_contract else '40ms latency/10ms jitter'}")
    except Exception:
        for log in logs: log.flush()
        for path in sorted(root.glob("*.log")): print(path.name,path.read_text(errors="replace")[-12000:])
        raise
    finally:
        if relay: relay.close()
        for peer in peers:
            if peer.poll() is None: peer.terminate(); peer.wait(timeout=5)
        for log in logs: log.close()
