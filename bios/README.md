# BIOS

Drop your own dump of the GBA BIOS here as `gba_bios.bin`. The binary
**is not in git** (it's copyrighted Nintendo code) but the `.toml` /
`.md` / `.sym` files in this folder ARE tracked, so the path layout
matches between developer machines.

## Why we need a real BIOS

`gbarecomp` runs the **actual** GBA BIOS, interpreted instruction by
instruction. We do **not** HLE SWIs. We do **not** stub the BIOS
intro. We do **not** "fast-forward through boot." The whole point of
a recompiler is faithfulness; the BIOS is part of what we recompile
through.

This matches the PSXRecomp approach — see `../docs/ARCHITECTURE.md`
"BIOS as the boot path" and `../PRINCIPLES.md` "BIOS is sacred."

## Required image

| Field    | Value                                                                 |
|----------|-----------------------------------------------------------------------|
| File     | `bios/gba_bios.bin`                                                   |
| Size     | 16384 bytes (16 KB)                                                   |
| SHA-1    | `300c20df6731a33952ded8c436f7f186d25d3492`                            |
| MD5      | `a860e8c0b6d573d191e4ec7db1b1e4f6`                                    |
| Origin   | Nintendo GBA system ROM (region-agnostic)                             |

The runtime refuses to start with a BIOS whose hash isn't recognized.
This is the same gate the cartridge ROM goes through; both must
verify.

## Where to put it

`F:/Projects/gbarecomp/gbarecomp/bios/gba_bios.bin`

The `bios_smoke` tool, the runtime, and `gba_recompile` all look here
by default. Override with `--bios <path>` if you keep yours elsewhere.

## What lives here that IS tracked

- `README.md` — this file.
- `gba_bios.toml` — audit / recompile config for the BIOS, matching
  the shape of a game's `game.toml`.
- `gba_bios.sym` — symbol map (when populated). Sources are public
  GBA BIOS disassemblies; see `../docs/GBA_REFERENCE_NOTES.md`
  "GBA BIOS references."

## What does NOT live here

- Decompiled or "lifted to C" BIOS source. We interpret the original
  bytes; we don't ship a re-implementation.
- HLE stubs that pretend to be SWIs.
