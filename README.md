# gbarecomp

Game-agnostic static recompiler for the Game Boy Advance (Nintendo GBA,
ARM7TDMI / ARMv4T, dual ARM+THUMB interworking).

This is the platform core. It is **not** a port, an HLE shim, or a
re-implementation of any specific game. It takes original GBA ROM
machine code (ARM and THUMB) and produces native C/C++ that executes
against a principled GBA hardware/runtime model.

The first concrete target lives in a sibling repo,
[`MinishCapRecomp`](../MinishCapRecomp), which uses this core to
recompile *The Legend of Zelda: The Minish Cap*.

---

## How to use GBARecomp

### Use the Windows CLI

1. Open the [Releases](https://github.com/mstan/gbarecomp/releases) page.
2. Download `gbarecomp-cli-windows-x86_64.zip`. Do not download GitHub's
   "Source code" archives.
3. Extract the zip to its own folder.
4. Open PowerShell in that folder.
5. Run:

```powershell
.\gbarecomp.exe build `
  --rom "C:\Games\MyGame.gba" `
  --output "C:\Projects\MyGameRecomp"
```

No BIOS file is needed to generate source. The output folder contains sharded
C++ source, runtime-facing headers, CMake files, and build scripts.

To check that the generated source compiles:

```powershell
cd "C:\Projects\MyGameRecomp"
.\build.ps1
```

This builds a static library. It does not create a playable game by itself.
Each game still needs game-specific configuration and runtime integration. Use
one of the [Game targets](#game-targets) as a starting point for a full port.

If you already have a game TOML or imported symbol file, add `--config` or
`--symbols` to the build command.

GBARecomp does not include a GBA BIOS or game ROMs.

---

## Game targets

Each target is its own repo that consumes this core. The three
Pokémon repos cover **five games** between them:

| Repo | Games |
|------|-------|
| [FireRedLeafGreenRecomp](https://github.com/mstan/FireRedLeafGreenRecomp) | Pokémon FireRed, Pokémon LeafGreen |
| [RubySapphireRecomp](https://github.com/mstan/RubySapphireRecomp) | Pokémon Ruby, Pokémon Sapphire |
| [EmeraldRecomp](https://github.com/mstan/EmeraldRecomp) | Pokémon Emerald |
| [MinishCapRecomp](https://github.com/mstan/MinishCapRecomp) | The Legend of Zelda: The Minish Cap |

---

## What this repo contains

| Folder        | Purpose                                                                 |
|---------------|-------------------------------------------------------------------------|
| `src/armv4t/` | Portable ARM7TDMI / ARMv4T decoder, IR, codegen. Reusable for ARM9.     |
| `src/gba/`    | GBA-specific bus, IO, IRQ, DMA, timers, PPU, audio, input, save, BIOS.  |
| `src/runtime/`| Linkable runtime: dispatch, host platform glue, generated-code support. |
| `src/debug/`  | TCP debug server, always-on ring buffers, snapshot/save-state.          |
| `tools/`      | `gba_scan`, `gba_recompile`, `symbol_import`, `test_rom_runner`.        |
| `tests/`      | Per-subsystem tests (decoder, bus, DMA, timers, IRQ, PPU smoke).        |
| `docs/`       | Architecture, GBA reference inventory, debugging, roadmap.              |
| `third_party/`| External libs (see `third_party/README.md` for licenses).               |

---

## Design boundary (read this before contributing)

1. **Recomp, not decomp.** We execute the original ROM's ARM/THUMB code
   through statically-translated C. We do not borrow PC-port renderers,
   PC-port audio mixers, or any high-level gameplay helper as the
   runner. Decomp projects are valuable references for symbols, ROM
   layout, and reverse-engineered notes — not as a source of execution
   behavior. See `docs/ARCHITECTURE.md` for the full list.
2. **No HLE-by-accident.** Every implemented hardware behavior must
   point at a documented GBA reference (GBATEK, CowBite, hardware test
   results) or a measured oracle. Stubs that happen to make Minish Cap
   work are forbidden.
3. **Hardware-faithful runtime.** PPU goes through VRAM/OAM/PAL; audio
   goes through FIFO/timer/DMA; saves go through the cartridge save
   chip model. Shortcuts that bypass hardware semantics are not
   acceptable.
4. **Generated C is evidence, not authority.** If generated code is
   wrong, fix the recompiler. Never edit `generated/*` by hand.
5. **Always-on observability.** All debug/tooling reads from ring
   buffers that record continuously. No "arm trace then run workload."
   See `docs/DEBUGGING.md` and `TCP.md`.

---

## Build

```
cmake -B build -S .
cmake --build build
```

To build the self-contained Windows CLI zip, install Python 3.12 and
PyInstaller, then run:

```powershell
py -3.12 -m pip install pyinstaller
py -3.12 tools/build_cli.py
```

The zip is written to `build/cli-release/`.

Cartridge recompilation always emits parallel `recompiled_NNN.cpp` shards; the
old giant `recompiled.cpp` form is no longer supported. On Windows, the supplied
Clang-MinGW toolchain and automatic ccache/sccache integration can reduce a
large clean game build from 10+ minutes to low single-digit minutes. See
[`docs/BUILD_PERFORMANCE.md`](docs/BUILD_PERFORMANCE.md).

Targets:

- `gba_scan` — walk a ROM and report header, entry, code regions.
- `gba_recompile` — produce generated C/C++ for a ROM + symbol set.
- `decoder_smoke` — feed known ARM/THUMB words and print IR.
- `armv4t_tests`, `thumb_tests`, `bus_tests`, `dma_tests`,
  `timers_tests`, `irq_tests`, `ppu_smoke_tests`.

---

## Opt-in expanded view

Game runners that implement their own safe scene/render policy may advertise a
logical width above the native 240 pixels with `RunOptions::max_view_width`.
Users then select a total width with `[video].view_width`, `--view-width`, or
`GBARECOMP_VIEW_WIDTH`; height remains 160. The default is always faithful
240x160, and requests for a game that has not opted in are clamped to 240. See
`ENHANCEMENTS.md` and `docs/TOML_SCHEMA.md` for the contract and legacy aliases.

---

## Reuse target

The `src/armv4t/` tree is intentionally portable. The reusable surface
is ARM/THUMB decode, condition codes, flags, interworking, block
discovery, IR, and codegen. The non-reusable surface is everything in
`src/gba/`. This separation is what lets `gbarecomp` later contribute
to an ARM9 / 3DS-style effort without contaminating either side.

For a strict static-coverage acceptance run, set
`GBARECOMP_STRICT_STATIC=1`. The runtime will not load cached overlays, compile
healing shards, or bridge a missing dispatch through the interpreter; the first
missing static PC aborts loudly. Pair it with BIOS HLE disabled when validating
the fully LLE path.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>
