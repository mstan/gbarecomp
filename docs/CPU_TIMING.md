# CPU timing model

Every guest instruction advances the master clock (`g_runtime_cycles`) by
its ARM7TDMI cost on the GBA bus. The static codegen, the reference
interpreter (self-heal bridge, force-interp co-sim, `bios_smoke`) and the
Stage-2 overlays share one model; the pinned mGBA (`1d201b22`) is the
reference and GBATEK the source for the constants.

## Cost of one instruction

```
instr_cycle_base(op)            fixed part: 1S fetch + internal cycles
                                (branch refill folded in), zero-wait fetches
+ 1 register-specified shift, + 2 non-branch PC write
+ data accesses                 GbaBus::access_cycles (N first, then S)
+ multiply m-cycles             mul_wait_cycles
+ opcode-fetch wait states      Bus::code_wait of the code region:
    S                           every instruction (also condition-failed)
    N instead of S              after LDR/STR/LDM/STM or a multiply
    N + S of the target         after any PC write, in the target's region
                                and instruction set (16-bit THUMB, 32-bit ARM)
+ GamePak prefetch adjustment   Bus::prefetch_stall, cartridge code only
```

`next_fetch_nonsequential` (arm-recomp-core `arm_ir.h`) lists the ops whose
next fetch is N. SWP/SWPB cost 1S+2N+1I and, like mGBA, get no N adjust.

## Wait states

`RuntimeWaitTable` (`runtime_arm_types.h`) holds, per region `addr >> 24`,
the N/S wait states for 16- and 32-bit accesses, the prefetch-enable bit and
the prefetch-buffer position. Each machine's bus owns one
(`gba_waitstates.{h,cpp}`); while a bus is active its table lives in the
runtime's `g_runtime_waits` (`GbaBus::bind_wait_table`), so generated code
reads it with a single load.

- **WAITCNT** (`04000204h`): SRAM 4/3/2/8, WS0-2 first access 4/3/2/8 and
  second access 2/1, 4/1, 8/1; bit 14 enables the prefetch buffer. Bits 13
  and 15 read as zero.
- **Internal memory control** (`04000800h`, mirrored every 64 KiB): EWRAM
  waits `15 - bits 24-27`, reset `0D000020h` (2 waits). A 0-wait setting
  (0Fh) is rejected and the previous one kept, as in mGBA.
- Fixed buses: BIOS/IWRAM/IO/OAM zero-wait 32-bit, palette/VRAM zero-wait
  16-bit (32-bit access +1).

Codegen constant-folds the fetch waits of fixed-bus code (BIOS, IWRAM) and
reads the table for EWRAM and cartridge code; run-time PC targets use the
inline `runtime_refill_cycles`. The table is part of every saved state
(snapshot `TAG_BUS` tail, multiplayer device state); WAITCNT itself is in
the IO page and is re-derived on load.

## Prefetch buffer

`rwt_prefetch_stall` (`runtime_wait_model.h`, used by the bus model and by
`runtime_prefetch_stall_delta`, which generated code calls while the buffer
is on) is mGBA's `GBAMemoryStall`: while cartridge code runs with
the buffer enabled, a data access below `08000000h` (or a multiply's
internal cycles) lets the buffer fetch sequential halfwords; the next fetch
turns from N into S and the prefetched fetches become free. Its position
(`last_prefetched_pc`) is timing state: the Stage-2 idle prover includes it
in the loop fixed point, and shadow re-runs (widescreen sidecar) evaluate on
a copy.

## Verification

| Test | Checks |
|---|---|
| `codegen_tests` | codegen vs interpreter cycles and prefetch position for the whole L1 corpus, and again relocated into cartridge ROM under five WAITCNT/EWRAM settings (655 runs) |
| `timing_rom_tests` | `tests/timing/timing_fixture.S`: 33 ARM/THUMB kernels from ROM, IWRAM and EWRAM under five settings (495 timer measurements), statically recompiled; hand-derived GBATEK anchors |
| `timing_mgba_differential` | the same 495 values equal the pinned mGBA exactly (`oracle/timing`, `-DGBARECOMP_TIMING_ORACLE_EXE`) |
| `bus_tests` | WAITCNT/memory-control decode, masking, mirrors, snapshot and multiplayer-state round trips |
| `link_rom_tests` | the link fixture's settle loop costs 38 cycles/iteration from ROM |

`oracle/timing/frame_budget.py` compares per-frame CPU budgets of real
cartridges (SWI logs of `bios_smoke` and `gba_timing_oracle --swi-log`).

## Known differences from mGBA

- **LDM/STM on cartridge space.** mGBA 1d201b22 `GBALoadMultiple` /
  `GBAStoreMultiple` (`src/gba/memory.c:1508`, `:1630`) start the burst at
  `S - N` wait states, so a ROM burst costs `2(N - S)` less than N followed
  by S. The native model keeps the hardware order (first access N) and the
  fixture does not exercise it.
- **PPU VRAM contention.** mGBA stalls VRAM/palette/OAM accesses while the
  PPU fetches (`GBAMemoryStallVRAM`, `stallMask`); the native bus does not.
  The fixture never touches those regions.
- **HLE BIOS.** Opt-in HLE SWIs charge mGBA-derived service costs plus the
  SWI instruction's own fetch; they are not part of the differential.

## History

Until this model, generated and interpreted code charged every opcode fetch
as a zero-wait 1S and ignored WAITCNT, so cartridge code ran up to several
times faster than hardware. The link fixture's ARM settle loop took 6 instead
of 38 cycles per iteration (first transfer at ~24.6k instead of mGBA's ~164k
cycles). Real cartridges were charged 4-27% too few busy cycles per frame
(Kirby, Metroid Fusion, Minish Cap; see beads-mc7.32).
