"""Compare separate native and mGBA processes; no emulator in native linkage."""
import subprocess
import sys

native, oracle, rom = sys.argv[1:]
def run(args):
    result = subprocess.run(args, check=True, capture_output=True, text=True, timeout=30)
    return result.stdout.splitlines()
for mode in ("multi", "normal"):
    expected = run([oracle, rom] + (["--normal"] if mode == "normal" else []))
    actual = run([native, rom, "--normal" if mode == "normal" else "--trace"])
    if len(expected) != 8 or actual != expected:
        raise AssertionError(f"{mode} differential mismatch\nnative: {actual}\nmGBA: {expected}")
print("native/mGBA agree: multiplayer all four baud rates, normal 8/32-bit both clocks, data/status/serial IRQ")
