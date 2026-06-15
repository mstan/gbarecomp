# gbaref

A standalone **libretro GBA reference oracle** for the gbarecomp recompiler —
the GBA sibling of [`snesref`](../../../snesrecomp) and `mdref` (Genesis).

It is a tiny SDL2 [libretro](https://www.libretro.com/) frontend that loads a
known-good GBA core (e.g. `mgba_libretro.dll`), plays a ROM with **recomp-matched
keybinds**, and logs a per-frame **WRAM-change trace** (IWRAM `0x03000000` +
EWRAM `0x02000000`) as JSONL — the same shape the gbarecomp runtime emits with
`GBARECOMP_WRAM_TRACE`. You diff the two traces with `oracle/ref_diff.py`.

## Why a separate libretro core, not the in-tree interpreter

The gbarecomp interpreter (`bios_smoke`) **shares `src/gba/*`** — the bus, DMA,
PPU, timers, save, and BIOS-protection models — with the recompiled runtime. So
for any device/bus/timing bug, the interpreter and the recomp diverge from real
hardware **identically**: recomp-vs-interp is blind to it. A real emulator core
(mGBA via libretro) is an **independent, hardware-faithful** oracle that can.

## Why this method beats savestate/scripted-input alignment

Aligning savestates across builds, or scripting input from a fresh boot, is
brittle and **always eventually fails** (boot timing, RNG, frame skew). Here a
human just **plays both sides to the same scene** — no scripting, no alignment.
The traces are diffed by **value and order**, not frame number
(recomp-template: "order + state + caller, not absolute frames").

## Build

```bash
PATH=/c/msys64/mingw64/bin:$PATH ./build.sh      # msys2 mingw64 + SDL2
```
Produces `gbaref.exe`. `SDL2.dll` (mingw) is alongside it.

## Run

```bash
gbaref.exe <core.dll> <rom.gba>
# e.g.  gbaref.exe mgba_libretro.dll minishcap_usa.gba
```
Drop a libretro GBA core DLL (e.g. `mgba_libretro.dll`) next to the exe. mGBA is
open source (MPL-2.0); the prebuilt core is a free download — it's just **not
committed here** (gitignored) so this repo carries no core's license terms:

```bash
curl -fsSL -o mgba.zip https://buildbot.libretro.com/nightly/windows/x86_64/latest/mgba_libretro.dll.zip
unzip mgba.zip && rm mgba.zip
```

mGBA wants the real GBA BIOS as `gba_bios.bin` in its system dir (= this dir);
copy `bios/gba_bios.bin` here for hardware-accurate boot (else it HLEs the BIOS).

### Keys (match the gbarecomp host window)

| Key | GBA | | Key | Action |
|-----|-----|-|-----|--------|
| Arrows | D-pad | | Enter | Start |
| Z | A | | RShift | Select |
| X | B | | F1–F9 | load state slot |
| S | L | | Shift+Fn | save state slot |
| A | R | | Backspace | clear trace |
| | | | Esc | quit |

### Env

- `GBAREF_TRACE` — output path (default `gbaref_trace.jsonl`)
- `GBAREF_WATCH_LO` / `GBAREF_WATCH_HI` — restrict traced GBA addresses
- `GBAREF_QUIT_FRAMES` — headless frame cap
- `GBAREF_SYSRAM_BASE` — base addr if the core sends no memory map (fallback)

## RAM parity (what's actually diff-able)

mGBA's libretro memory map exposes the whole address space, but only the
**work-RAM** regions are written through live and are therefore diff-able:

| Region | addr | mGBA memmap flag | live & diff-able |
|--------|------|------------------|------------------|
| EWRAM  | 0x02000000 (256K) | 0x4 (RAM) | ✅ yes |
| IWRAM  | 0x03000000 (32K)  | 0x4 (RAM) | ✅ yes |
| IO / PAL / VRAM / OAM | 0x04–0x07 | 0x0 | ❌ registered but **not** live via memmap |
| BIOS / ROM | 0x00 / 0x08+ | const | n/a |

**EWRAM + IWRAM are at full live parity with the recomp's
`GBARECOMP_WRAM_TRACE`** — and that is where all game *state/logic* lives
(objects, entity tables, the MC-HP-002 animation object). IO/PAL/VRAM/OAM are
output/register state that the mGBA core doesn't surface as live memory, so they
are out of scope here (and irrelevant to logic/state bugs). For graphics-only
divergences, VRAM could later be pulled via `RETRO_MEMORY_VIDEO_RAM`.

So: diff within `0x02000000-0x03007fff` (use `*_WATCH_LO/_HI` on both sides).

## Workflow

1. Reference: `GBAREF_TRACE=ref.jsonl gbaref.exe mgba_libretro.dll rom.gba`,
   play to the suspect scene. (Use F-key save/load to park at it.)
2. Recomp: `GBARECOMP_WRAM_TRACE=rec.jsonl MinishCapRecomp.exe ...`, play the
   same scene.
3. Diff:
   ```bash
   python oracle/ref_diff.py rec.jsonl ref.jsonl --final --watch 0x03001700-0x030017ff
   python oracle/ref_diff.py rec.jsonl ref.jsonl --addr 0x0300174a
   ```
   The address whose final value / value-sequence diverges is the corrupt state;
   trace its writer (WRONG value) or the producing code that never ran (MISSING).
