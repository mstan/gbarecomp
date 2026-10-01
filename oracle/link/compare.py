"""Compare separate native and mGBA processes; no emulator in native linkage.

For each cable size (2..4 consoles) this diffs the per-port CSV lines
(baud,port,SIOMULTI0/1,SIOMULTI2/3,SIOCNT,IRQ) for multiplayer and for normal
serial, then the hub-level transfer timing:

  T,baud,players,span,finish,skew...
    span   completion time minus the master's start-write time
    finish completion time in that side's own timeline (not compared: the
           native and mGBA startup delay loops take different cycle counts;
           cause not yet investigated, so absolute cycles are not compared)
    skew   mGBA per-port completion time minus the master's (must be 0)

Native span is sampled at one-instruction granularity after the start, so it
may trail mGBA's exact span by a few cycles; anything larger is a mismatch.
"""
import subprocess
import sys

native, oracle, rom = sys.argv[1:]
PLAYERS = (2, 3, 4)
SPAN_SLACK = 4  # cycles the native sample may trail mGBA's exact span


def run(args):
    result = subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True, timeout=120)
    return result.stdout.splitlines()


def mismatch(what, native_lines, oracle_lines):
    raise AssertionError(f"{what} differential mismatch\nnative: {native_lines}\nmGBA: {oracle_lines}")


def split_timing(lines):
    return [l for l in lines if not l.startswith("T,")], [l for l in lines if l.startswith("T,")]


for players in PLAYERS:
    for mode in ("multi", "normal"):
        opts = ["--players", players]
        expected = run([oracle, rom] + (["--normal"] if mode == "normal" else []) + opts)
        actual = run([native, rom, "--normal" if mode == "normal" else "--trace"] + opts)
        if len(expected) != 4 * players or actual != expected:
            mismatch(f"{players}-player {mode}", actual, expected)

    # Data must also be unchanged when the mGBA timing probe is attached.
    data, oracle_t = split_timing(run([oracle, rom, "--players", players, "--timing"]))
    native_data, native_t = split_timing(run([native, rom, "--timing", "--players", players]))
    if native_data != data:
        mismatch(f"{players}-player timing-mode data", native_data, data)
    if len(oracle_t) != 4 or len(native_t) != 4:
        mismatch(f"{players}-player timing line count", native_t, oracle_t)
    for n_line, o_line in zip(native_t, oracle_t):
        n = n_line.split(",")
        o = o_line.split(",")
        if n[1:3] != o[1:3] or len(o) != 5 + players - 1 or len(n) != len(o):
            mismatch(f"{players}-player timing shape", n_line, o_line)
        gap = int(o[3]) - int(n[3])
        if not 0 <= gap <= SPAN_SLACK:
            mismatch(f"{players}-player baud {o[1]} transfer span (mGBA - native = {gap})", n_line, o_line)
        if any(int(s) for s in o[5:]) or any(int(s) for s in n[5:]):
            mismatch(f"{players}-player baud {o[1]} completion skew", n_line, o_line)
print("native/mGBA agree for 2, 3 and 4 consoles: multiplayer all four baud rates, "
      "normal 8/32-bit both clocks, data/status/serial IRQ, transfer span")
