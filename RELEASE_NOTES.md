# gbarecomp v0.1.0

First tagged cut of the static GBA recompiler. Phase 2.8.D milestone:
the recompiled GBA BIOS executes via `runtime_dispatch` on the host C
stack with the interpreter strictly off the hot path. Minish Cap reaches
its title screen on top of this BIOS path (see MinishCapRecomp v0.0.1).

## What's in this release

- `bios_smoke.exe` — Phase 2.1 BIOS canary. Loads your GBA BIOS dump,
  hash-verifies it (CRC32 + SHA-1, warn-and-try), then steps the
  interpreter through the boot ROM. Useful for confirming a dump is
  valid before wiring it into a game runner. The interpreter here is
  the offline oracle use permitted by `PRINCIPLES.md` — it is not the
  runtime hot path.
- `bios.cfg` is written next to `bios_smoke.exe` after a successful
  pick so you don't have to pick again on relaunch.
- `SDL2.dll`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`
  — runtime dependencies (MSYS2 mingw64 build).
- `LICENSE` — PolyForm Noncommercial 1.0.0.
- `START_HERE.txt` — first-launch instructions.

## What's NOT in this release

- **No GBA BIOS image.** The 16 KiB `gba_bios.bin` is copyrighted by
  Nintendo. You provide your own dump. The expected SHA-1 is
  `300c20df6731a33952ded8c436f7f186d25d3492` and the expected CRC32 is
  `0x21A2AE0A`. The picker warns on mismatch but lets you try anyway.
- **No game ROMs.** Game runtimes (Minish Cap, etc.) ship separately.
- **No generated C.** The recompiler output is regenerated from your
  BIOS dump at build time when you build from source.

## First launch

1. Run `bios_smoke.exe`.
2. A Windows file picker appears. Select your `gba_bios.bin`.
3. The runtime hash-verifies the BIOS. If hashes match, the BIOS smoke
   test runs and prints a short trace. If they don't, you'll see a
   warning dialog quoting the actual + expected hashes; the smoke test
   still runs so you can validate atypical dumps.
4. The validated path is saved in `bios.cfg` next to the .exe.

To pick a different BIOS later, delete `bios.cfg` or pass `--bios`
explicitly.

## Highlights since branch creation

- `arm_codegen` pins ARMv4T edge cases: PC+12 reads for
  register-controlled shifts, S-bit LDM/STM with PC absent, LDMIA with
  base in list, empty-list with writeback, STMDB writeback with base
  not first in list.
- Runtime: direct-call return idiom (host C-stack BLs with a
  return-stack predicate), always-on `RuntimeTraceEntry` ring buffer
  for post-mortem diagnostics, HALT idle pump that ticks PPU / timer /
  audio in cycle chunks until HALTCNT clears.
- `function_finder` keys functions by `(addr, mode)` so ARM and THUMB
  can coexist at the same address, supports `[[code_copy]]`
  declarations for IWRAM / EWRAM copies whose source bytes live in
  ROM, and seeds fall-through after conditional indirect transfers
  (`bxeq lr` falls through to the next instruction when the condition
  fails).
- BIOS TOML adds five new `extra_func` entries plus a 30-entry
  `bios_intro_command` jump table at `0x3738`.
- `[[code_copy]]` schema documented in `docs/TOML_SCHEMA.md`.
- Asset picker module (this release): Win32 file dialog +
  CRC32/SHA-1 validation + sidecar cache.

## Known issues

- The picker's Win32 file dialog is Windows-only. On Linux / macOS,
  pass `--bios` explicitly until a portable dialog lands.
- See `ISSUES.md` for the deferred-bug list (audio sample drift,
  bios_intro_flawless ctest, etc.).
