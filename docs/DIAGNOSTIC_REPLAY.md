# Diagnostic replays

Interactive windowed play records a **diagnostic replay** by default: the
host inputs that reached the guest at every frame boundary, plus fingerprints of
the machine each frame produced. When a player reports a bug, the recording lets
a developer (or an agent) rerun the exact session offline with the same build and
their own ROM/BIOS. Playback stops at the **first frame whose state differs** from
the recording and saves that machine.

The design follows Mega Man X (snesrecomp) and its successor's lesson. The first
version there wrote and hashed on the simulation thread and kept only a minute of
history. That caused lag and lost the bug. Here the simulation thread only copies
bytes into a fixed queue, and a worker does all hashing and I/O. One hour of
history is retained within a bounded budget.

## For players

- **On by default.** Recording starts automatically in windowed play. Files go to
  `diagnostics/replays/` beside the executable. For the Linux AppImage this is
  `~/.local/share/<Game>/diagnostics/replays/`.
- **Reporting a bug.** Quit normally, then zip every file from that session:
  `<Game>-<date>-<time>-<pid>-NNNNNN.replay` together with all of its sidecars
  (`.state`, `.config.ini`, `.game.toml`, `.mods.toml`, `.sav`, `.args.txt`).
  A `.replay` file alone is not enough. Say roughly when the problem happened.
  The ROM and BIOS are never recorded.
- **Budget.** Up to one hour is kept: 360 segments of 600 frames, with at most
  256 MiB of machine checkpoints. Older segments are deleted as you play.
- **Recording control.** Open **Mods → Gameplay Diagnostics → Record Gameplay**.
  Recording is on by default; the saved On or Off choice is independent of
  presentation mods. Existing recording preferences are imported when the
  control first becomes available, without replacing an explicit new choice.

## For developers and agents: reproduce a report

Titles include a targeted manifest for the shared
`gbarecomp.feature.gameplay-diagnostics` / `record-gameplay` binding, with
`default_enabled = true`. The engine binds it to the existing recorder API;
no title recorder or presentation plugin is needed. Netplay lobby commits
preserve this host preference without activating gameplay mods or replacing
offline gameplay selections. A read-only state lookup is available without
loading a catalog. This does not add replay capture to the netplay backend.
For explicit automated runs, `GBARECOMP_DIAGNOSTICS=0/1` still overrides the
selection. Older runners without the control retain their configuration fallback.

```sh
python tools/replay_diagnostic.py check  <first .replay>
python tools/replay_diagnostic.py replay <first .replay> \
    --exe <same build>/Game.exe --rom <rom> --bios <gba_bios.bin> [--window]
```

`replay` sets up a private reproduction directory:

1. It copies the game directory, so the recorded `config.ini` and
   `mods/state.toml` sit beside the executable.
2. It restores the session's starting battery save.
3. It rebuilds the recorded command line.
4. It runs the session headless (or windowed with `--window`).

Playback shares the build's `recomp_cache`. The first replay of a report
compiles any self-healed functions the player's session ran natively. That takes
about a second each and needs the build's compiler; later replays reuse the
cache.

Exit status:

| Status | Meaning |
|---|---|
| 0 | Every recorded frame reproduced |
| 3 | First divergence, or an interrupted recording |
| 4 | Stopped early |
| 2 | Setup error |

Useful options:

- `--until N` stops after N recorded frames.
- `--dump-frames A-B` writes full machine snapshots at those frame boundaries:
  `*-frame-N.replay.state`.
  - Load them with `--load-state` or the TCP `savestate_load` command and debug
    from there.
  - Setting the same variable `GBARECOMP_DIAGNOSTICS_DUMP_FRAMES=A-B` during a
    capture writes `*.record.state` files. You can then diff a live machine
    against a replayed one section by section.

Reading the result:

- **`replay MATCHED ... ended normally`.** The whole session reproduced. If the
  player saw a bug, it is now reproducible at will.
- **`FIRST DIVERGENCE after recorded frame N ... EWRAM=DIFFERS IWRAM=match ...`.**
  The named regions differ after frame N, and `replay-divergence.state` holds
  the replayed machine. The usual causes are:
  - a different build (the header and the startup warning say so);
  - a nondeterminism bug in the runtime: something guest-visible that was not
    recorded. That is a bug worth fixing.
- **`INCOMPLETE ... the final recorded input has no fingerprint`.** The player's
  last frame never finished: a hang, crash or kill. Playback executed that
  input, so the fault should recur right there. Run it under a debugger, or use
  `--dump-frames` on the frames just before it.
- **`note: frame N matches; host bookkeeping differs (call-stack)`.** This is
  expected after playback resynchronizes at a checkpoint by redispatching. The
  runtime's call-return stack is host bookkeeping, so it is fingerprinted but
  never counted as a divergence.
- **`frame N reached at the present hook but recorded at the runner loop`.** The
  guest reached that frame through a different execution path, which is an
  early divergence signal.

Playback can start from any retained segment, not only the first, because
every segment begins with a full checkpoint.

## What is recorded

Inputs are sampled at the guest frame boundary, the one point that is identical
live and in playback. That boundary is one of:

- the present-in-place VBlank hook;
- the runner loop, for HALT-driven frames.

Mid-frame host polls never change guest-visible input while capturing. The
following are recorded per frame:

- KEYINPUT (the register the guest reads)
- the gyro sample offset
- solar sensor brightness
- the RTC host clock
- the view geometry, including adaptive widescreen width and margins

Each frame's fingerprint covers EWRAM, IWRAM, CPU registers, I/O, the guest
call-return stack, VRAM, palette, OAM and cartridge save memory.

Self-heal adoption is recorded too. Interpreter-bridged and native execution of
an uncovered function are not cycle-identical, so the moment a background
compile starts running natively is part of the session's history:

- During capture, compiled functions are installed only at frame boundaries.
  An `O` record logs each adoption, and every segment lists the complete native
  set in an `N` record.
- Playback installs exactly that set at the same boundaries, compiling
  synchronously if its own cache lacks a function. It never adopts code on its
  own.

Savestate loads, rewinds and suspend-resume restores are recorded as `R`
checkpoints and replayed at the same boundary. A restore inside the
present-in-place hook unwinds the guest and redispatches it, both live and in
playback, so the restored machine never resumes in a stale host call chain.

### Record format

Files are text, one record per line. `<file>.state` holds the checkpoint blobs.

```
GBARECOMP-REPLAY 1
BUILD <engine revision> <executable hash> <title>
ROM <sha1>
SEGMENT <n> <unix time>
S <offset> <size> <hash> <C|R>   checkpoint (C: live continued, R: live restored)
T <cycles> <vblanks> <yielded> <pending> <budget>   host scheduling at it
N <heal key>...                  complete self-healed native set
O <heal key>...                  native code adopted at this boundary
I <frame> <keys> <gyro> <solar> <rtc> <w> <h> <l> <r> <t> <b> <h|l>
H <frame> <hash per region>... <save hash>
X <frame>                        frame cut short by a mid-frame restore
G                                frames lost to a storage gap
E <frame>                        clean exit
```

## Cost and failure behavior

- **Producer side.** The simulation thread copies about 0.4 MB per frame into
  a fixed 32 MiB single-producer queue and serializes one checkpoint per
  10-second segment. Measured on Mega Man Zero 2 at about 520 fps uncapped, the
  overhead was within noise (0.1%) with no gaps.
- **Worker side.** The worker hashes the copies, writes files and copies the
  sidecars. It flushes every 100 ms, so a crash loses at most that much.
- **When storage falls behind.** The current segment is abandoned and marked
  `G`. Capture resumes with a fresh checkpoint once the queue drains, and
  gameplay never waits. Playback resynchronizes at the next checkpoint.
- **On exit.** A clean exit waits briefly for the writer and ends with `E`, so a
  missing `E` means the session was interrupted.

## Limits

- Not recorded:
  - link-cable and netplay sessions, which run on a different runner;
  - TCP-debugger sessions;
  - runs with `GBARECOMP_PRESENT_IN_PLACE=0`;
  - runs with validation cheats (`GBARECOMP_CHEATS`).
- Playback must use the same build. The header records the engine revision and
  executable hash, and a mismatch is reported up front.
- Hosts without threads (single-threaded wasm) do not capture.

## Validation-only helpers

These must never be used in normal play.

- **`GBARECOMP_CHEATS="ADDR:BITS:VALUE;..."`** (hex) writes RAM at every frame
  start, GameShark-style constant codes. It is used for cheat-assisted coverage
  runs and disables capture.
- **TCP `write_mem {"addr":A,"hex":"..."}`** writes raw bytes into EWRAM, IWRAM,
  palette, VRAM or OAM.
- **`GBARECOMP_DIAGNOSTICS_TEST_WRITER_DELAY_US`** simulates slow storage
  (acceptance tests of gap recovery).
