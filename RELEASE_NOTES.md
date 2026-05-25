# gbarecomp v0.1.0

First tagged cut of the static GBA recompiler. Phase 2.8.D milestone:
the recompiled GBA BIOS executes via `runtime_dispatch` on the host C
stack with the interpreter strictly off the hot path. Minish Cap reaches
its title screen on top of this BIOS path (see MinishCapRecomp v0.0.1).

## What you download

A single, fully-standalone `gba.exe`. No zip, no sidecar DLLs — SDL2,
libstdc++, libgcc, and libwinpthread are all statically linked into
the binary. The only DLLs it imports are stock Windows ones
(KERNEL32, USER32, GDI32, ole32, comdlg32, etc.) that ship with every
Windows install.

## First launch

1. Run `gba.exe`.
2. A Windows file picker appears. Select your `gba_bios.bin` dump.
3. The runtime hash-verifies the BIOS:
   - Match (SHA-1 `300c20df...3492`, CRC32 `0x21A2AE0A`) → BIOS canary
     runs.
   - Mismatch → warning dialog quoting the actual + expected hashes,
     then the canary runs anyway so you can validate atypical dumps.
4. The validated path is saved in `bios.cfg` next to the .exe.

To pick a different BIOS later, delete `bios.cfg` or pass `--bios`
explicitly on the command line.

## What's NOT in this release

- **No GBA BIOS image.** The 16 KiB `gba_bios.bin` is copyrighted by
  Nintendo. You provide your own dump.
- **No game ROMs.** Game runtimes (Minish Cap, etc.) ship separately.
- **No generated C.** The recompiler output is regenerated from your
  BIOS dump at build time when you build from source.

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
- Asset picker module: Win32 file dialog + CRC32/SHA-1 validation +
  sidecar cache.
- Static-release build flow: `-DGBARECOMP_STATIC_RELEASE=ON` plus
  `CMAKE_EXE_LINKER_FLAGS=-static -static-libgcc -static-libstdc++`
  produces a zero-dependency .exe.

## Known issues

- The picker's Win32 file dialog is Windows-only. On Linux / macOS,
  pass `--bios` explicitly until a portable dialog lands.
- The interpreter inside `gba.exe` is a deliberate offline oracle (it
  is what the BIOS canary uses to step instructions, per
  `PRINCIPLES.md`). It is NOT used in any game runner's hot path.
- See `ISSUES.md` for the deferred-bug list (audio sample drift,
  bios_intro_flawless ctest, etc.).
