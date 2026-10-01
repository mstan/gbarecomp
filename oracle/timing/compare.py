"""CPU timing differential: recompiled fixture vs the pinned mGBA.

Runs tests/timing's native runner (--csv) and oracle/timing's separate mGBA
frontend on the same original fixture ROM and requires every timer-measured
kernel cycle count to match exactly. No emulator code is linked natively.
"""
import subprocess
import sys

PASSES = ("power-on 4/2", "3/1 prefetch", "3/1", "2/1 prefetch", "4/2 prefetch EWRAM1")
REGIONS = ("ROM", "IWRAM", "EWRAM")
KERNELS = (
    "a_alu", "a_loop", "a_ldr_iwram", "a_ldr_ewram", "a_ldr_rom", "a_ldrh_io",
    "a_ldrb_sram", "a_str_iwram", "a_strh_ewram", "a_ldm_stm_iwram",
    "a_mul_small", "a_mul_large", "a_umull_smlal", "a_swp", "a_bl",
    "a_bx_thumb", "a_cond_fail", "a_alu_pc", "a_ldr_iwram_mul",
    "t_alu", "t_loop", "t_ldr_iwram", "t_ldr_ewram", "t_ldr_rom", "t_ldrh_io",
    "t_str_iwram", "t_push_pop", "t_mul", "t_bl", "t_bl_pop_pc", "t_bx_arm",
    "t_ldr_pcrel", "t_cond_branch",
)


def run(args):
    result = subprocess.run([str(a) for a in args], check=True,
                            capture_output=True, text=True, timeout=120)
    return [tuple(map(int, line.split(","))) for line in result.stdout.split()]


def label(index):
    per_pass = len(REGIONS) * len(KERNELS)
    p, rest = divmod(index, per_pass)
    r, k = divmod(rest, len(KERNELS))
    return f"{PASSES[p]} / {REGIONS[r]} / {KERNELS[k]}"


native, oracle, rom = sys.argv[1:]
actual = run([native, rom, "--csv"])
expected = run([oracle, rom])
count = len(PASSES) * len(REGIONS) * len(KERNELS)
if len(expected) != count or len(actual) != count:
    raise AssertionError(f"measurement count: native {len(actual)}, mGBA {len(expected)}, want {count}")
mismatches = [(i, a, e) for (i, a), (_, e) in zip(actual, expected) if a != e]
for i, a, e in mismatches[:40]:
    print(f"[{i}] {label(i)}: native {a} mGBA {e} (native - mGBA = {a - e})")
if mismatches:
    raise AssertionError(f"{len(mismatches)} of {count} kernel timings differ from mGBA")
print(f"native/mGBA agree on all {count} kernel timings: ARM/THUMB from ROM, IWRAM "
      "and EWRAM under five WAITCNT/EWRAM settings incl. the prefetch buffer")
