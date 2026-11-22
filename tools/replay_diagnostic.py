#!/usr/bin/env python3
"""Inspect or replay a gbarecomp diagnostic replay (docs/DIAGNOSTIC_REPLAY.md).

  replay_diagnostic.py list <diagnostics/replays dir>
  replay_diagnostic.py check <segment.replay>
  replay_diagnostic.py replay <segment.replay> --exe <game exe> --rom <rom> --bios <bios>
                       [--work DIR] [--window] [--until FRAMES] [--dump-frames A-B]

`replay` stages a private copy of the game directory (so the recorded
config.ini and mods/state.toml sit beside the executable exactly as they did
for the player), restores the session's starting battery save, rebuilds the
recorded command line, and runs the build headless (or windowed) until the
recording ends or the FIRST divergent frame. Exit status: 0 matched, 3
divergence or interrupted recording, 4 stopped early, 2 setup error.
Python 3.8+, standard library only.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SEGMENT = re.compile(r"^(?P<stem>.*)-(?P<n>\d{6})\.replay$")
SIDECARS = (".state", ".config.ini", ".game.toml", ".mods.toml", ".sav", ".args.txt")
# Command-line flags that name host files or session control; replay supplies
# its own. The value of each listed value-flag is dropped with it.
DROP_VALUE_FLAGS = {"--rom", "--bios", "--save", "--save-path", "--state-dir", "--tcp",
                    "--tcp-observe", "--load-state", "--frames", "--steps", "--config",
                    "--replay-diagnostic", "--dump-bmp", "--dump-png", "--dump-state", "--scale",
                    "--rom-sha1", "--rom-crc32", "--bios-sha1"}
DROP_FLAGS = {"--launcher", "--no-launcher", "--window", "--no-window", "--fullscreen",
              "--uncapped"}
# Every runtime flag that consumes the next argument (runtime.cpp kCliValueFlags).
VALUE_FLAGS = DROP_VALUE_FLAGS | {"--view-width", "--widescreen", "--gyro-sensitivity",
                                  "--sharp-filter", "--linear-filter", "--affine-filter",
                                  "--screen", "--volume"}


def chain(first: Path):
    """The given segment and every consecutive segment after it."""
    m = SEGMENT.match(first.name)
    if not m:
        raise ValueError(f"{first.name} is not a <stem>-NNNNNN.replay segment")
    n = int(m["n"])
    out = []
    while True:
        seg = first.with_name(f"{m['stem']}-{n:06d}.replay")
        if not seg.is_file():
            return out
        out.append(seg)
        n += 1


def inspect(segments):
    """Summarize records; returns (header lines, stats dict)."""
    stats = {"frames": 0, "checkpoints": 0, "restores": 0, "gaps": 0, "adoptions": 0,
             "ended": False, "dangling": False, "first_frame": None, "last_frame": None}
    header = None
    for seg in segments:
        lines = seg.read_text(encoding="utf-8").splitlines()
        if len(lines) < 4 or lines[0] != "GBARECOMP-REPLAY 1":
            raise ValueError(f"{seg.name}: unsupported or truncated header")
        header = header or lines[1:4]
        if lines[1:3] != header[0:2]:
            raise ValueError(f"{seg.name}: BUILD/ROM differ from the first segment")
        if not seg.with_name(seg.name + ".state").is_file():
            raise ValueError(f"{seg.name}: missing .state checkpoint file")
        pending = False
        for line in lines[4:]:
            kind = line[:1]
            if kind == "S":
                stats["checkpoints"] += 1
                stats["restores"] += line.endswith(" R")
            elif kind == "I":
                pending = True
                frame = int(line.split()[1])
                if stats["first_frame"] is None:
                    stats["first_frame"] = frame
            elif kind == "H":
                pending = False
                stats["frames"] += 1
                stats["last_frame"] = int(line.split()[1])
            elif kind == "X":
                pending = False
            elif kind == "G":
                pending = False
                stats["gaps"] += 1
            elif kind == "O":
                stats["adoptions"] += len(line.split()) - 1
            elif kind == "E":
                pending = False
                stats["ended"] = True
            elif kind not in ("T", "N"):
                raise ValueError(f"{seg.name}: unknown record {line[:40]!r}")
        stats["dangling"] = pending
    return header, stats


def cmd_list(args):
    sessions = {}
    for seg in sorted(Path(args.dir).glob("*.replay")):
        m = SEGMENT.match(seg.name)
        if m:
            sessions.setdefault(m["stem"], []).append(int(m["n"]))
    for stem, ns in sessions.items():
        print(f"{Path(stem).name}: segments {min(ns)}..{max(ns)} ({len(ns)})")
    return 0


def cmd_check(args):
    segments = chain(Path(args.segment).resolve())
    header, s = inspect(segments)
    print("\n".join(header))
    print(f"segments={len(segments)} frames={s['frames']} (frame {s['first_frame']}.."
          f"{s['last_frame']}) checkpoints={s['checkpoints']} restores={s['restores']} "
          f"gaps={s['gaps']} native_adoptions={s['adoptions']}")
    if s["ended"]:
        print("The session ended normally.")
    elif s["dangling"]:
        print("The final input has no fingerprint: that frame hung, crashed or the "
              "process was killed. Replaying executes it.")
    else:
        print("No end record: the capture stopped without a clean exit.")
    return 0


def recorded_args(path: Path):
    """The player's command line minus host paths and session control."""
    if not path.is_file():
        return []
    argv = path.read_text(encoding="utf-8").splitlines()[1:]
    out, skip = [], False
    for a in argv:
        if skip:
            skip = False
            continue
        flag = a.split("=", 1)[0]
        if flag in DROP_VALUE_FLAGS:
            skip = "=" not in a
            continue
        if flag in DROP_FLAGS:
            continue
        if not a.startswith("-") and (not out or out[-1] not in VALUE_FLAGS):
            continue  # positional game config: replaced by the recorded copy
        out.append(a)
    return out


def cmd_replay(args):
    first = Path(args.segment).resolve()
    segments = chain(first)
    header, stats = inspect(segments)
    print("\n".join(header))
    exe = Path(args.exe).resolve(strict=True)
    work = Path(args.work).resolve() if args.work else Path(tempfile.mkdtemp(prefix="gbarecomp-replay-"))
    work.mkdir(parents=True, exist_ok=True)
    app = work / "app"
    if app.exists():
        raise ValueError(f"{app} already exists; choose a new --work directory")
    print(f"Reproduction directory: {work}", flush=True)
    # A private copy of the game directory: mods/state.toml and config.ini are
    # read from beside the executable, exactly as for the player.
    shutil.copytree(exe.parent, app, ignore=shutil.ignore_patterns(
        "diagnostics", "recomp_cache*", "saves", "*.sav", "*.state*", "*.log",
        "replay-divergence.state"))
    rec = work / "recording"
    rec.mkdir()
    for seg in segments:
        for suffix in ("",) + SIDECARS:
            src = seg.with_name(seg.name + suffix)
            if src.is_file():
                shutil.copy2(src, rec / src.name)
    staged = rec / first.name
    base = first  # sidecars describe the session as of this segment
    if base.with_name(base.name + ".config.ini").is_file():
        shutil.copy2(base.with_name(base.name + ".config.ini"), app / "config.ini")
    if base.with_name(base.name + ".mods.toml").is_file():
        (app / "mods").mkdir(exist_ok=True)
        shutil.copy2(base.with_name(base.name + ".mods.toml"), app / "mods" / "state.toml")
    save = work / "replay.sav"
    if base.with_name(base.name + ".sav").is_file():
        shutil.copy2(base.with_name(base.name + ".sav"), save)
    if args.until is not None:
        truncate(rec, segments, args.until)
    command = [str(app / exe.name), "--no-launcher", "--replay-diagnostic", str(staged),
               "--rom", str(Path(args.rom).resolve(strict=True)),
               "--bios", str(Path(args.bios).resolve(strict=True)),
               "--save-path", str(save)]
    command += recorded_args(base.with_name(base.name + ".args.txt"))
    game_toml = base.with_name(base.name + ".game.toml")
    if game_toml.is_file():
        shutil.copy2(game_toml, work / "game.toml")
        command.append(str(work / "game.toml"))
    if not args.window:
        command.append("--no-window")
    env = dict(os.environ)
    env["GBARECOMP_DIAGNOSTICS"] = "0"
    env["GBARECOMP_NO_LAUNCHER"] = "1"
    env["GBARECOMP_DIAGNOSTICS_DIR"] = str(work)
    # Share the build's self-heal cache (keyed by ROM and code CRC): playback
    # makes the recorded native set active and only compiles what is missing.
    env.setdefault("GBARECOMP_HEAL_CACHE", str(exe.parent / "recomp_cache"))
    if args.dump_frames:
        env["GBARECOMP_DIAGNOSTICS_DUMP_FRAMES"] = args.dump_frames
    print("Command:", " ".join(command), flush=True)
    with (work / "replay.log").open("wb") as log:
        result = subprocess.run(command, cwd=work, env=env, stdout=log,
                                stderr=subprocess.STDOUT)
    text = (work / "replay.log").read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        if "[gbarecomp:diagnostics]" in line or "self_heal_coverage" in line:
            print(line)
    print(f"exit status {result.returncode}; full log: {work / 'replay.log'}")
    return result.returncode


def truncate(rec: Path, segments, frames: int):
    """Keep the first `frames` fingerprinted frames, then end cleanly."""
    seen = 0
    for i, seg in enumerate(segments):
        path = rec / seg.name
        lines = path.read_text(encoding="utf-8").splitlines()
        for j, line in enumerate(lines):
            if line.startswith("H "):
                seen += 1
                if seen == frames:
                    path.write_text("\n".join(lines[:j + 1] + [f"E {line.split()[1]}"]) + "\n",
                                    encoding="utf-8")
                    for later in segments[i + 1:]:
                        for f in rec.glob(later.name + "*"):
                            f.unlink()
                    return
    raise ValueError(f"--until {frames} exceeds the {seen} recorded frames")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("dir")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("check")
    p.add_argument("segment")
    p.set_defaults(fn=cmd_check)
    p = sub.add_parser("replay")
    p.add_argument("segment")
    p.add_argument("--exe", required=True)
    p.add_argument("--rom", required=True)
    p.add_argument("--bios", required=True)
    p.add_argument("--work")
    p.add_argument("--window", action="store_true", help="watch instead of running headless")
    p.add_argument("--until", type=int, help="stop after this many recorded frames")
    p.add_argument("--dump-frames", help="A-B: save full machine states at these frames")
    p.set_defaults(fn=cmd_replay)
    args = parser.parse_args()
    try:
        return args.fn(args)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
