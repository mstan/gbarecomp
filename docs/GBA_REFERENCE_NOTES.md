# GBA Reference Notes

This file inventories the external references we consult and is
explicit about what each one can and cannot be used for. Read this
before pulling text/code/behavior from any of these sources.

The hard rule: **emulators and hardware test ROMs are oracles;
decompilations are symbol/structure references, never execution
truth.**

---

## Primary hardware references (text)

### GBATEK
- Source: Martin Korth's GBATEK (multiple mirrors; primary at
  problemkaputt.de/gbatek.htm).
- Use for: register maps, IO addresses, bit field semantics, BIOS
  call list, memory map, waitstates, prefetch behavior, DMA timing,
  IRQ vector layout, save-chip behavior.
- License caveat: GBATEK text is not freely re-licensable. Cite,
  don't copy.

### Tonc (Tonc's GBA Programming Tutorial)
- Source: gbadev.org / coranac.com.
- Use for: practical hardware behavior, BG/affine math, palette
  layout, sample-correct examples for what code "looks like."
- Caveat: Tonc explains how to write GBA code; we are not writing
  GBA code, we are interpreting it. Use for sanity-checking, not
  authority.

### CowBite Spec
- Source: cs.rit.edu / cowbite mirror.
- Use for: register maps, especially historical disagreements with
  GBATEK on rare bits. Cross-reference with mGBA source when GBATEK
  and CowBite differ.

---

## Emulators (treat as oracles when accuracy-validated)

### mGBA — primary oracle
- Source: github.com/mgba-emu/mgba.
- Why primary: actively maintained, accurate, has scripting and GDB
  debugging hooks. We will build a bridge that exposes registers /
  memory / framebuffer / IO over our TCP convention.
- Use for: side-by-side execution comparison. The mGBA bridge lives
  next to our native build, exposes the matching `emu_*` commands,
  and is what `framebuf_diff` and `frame_diff` compare against.
- License: MPL 2.0. We do not statically link mGBA into our native
  build; we link it into a separate **oracle** binary (the bridge).

### NanoBoyAdvance — accuracy cross-check
- Source: github.com/nba-emu/NanoBoyAdvance.
- Why: very high accuracy, useful when mGBA says A and we observe B
  — a tiebreaker. Run as a second oracle, not as the primary.

### SkyEmu — automation candidate
- Source: github.com/skyline-emu/SkyEmu (or its public mirror).
- Why: has a documented REST/debug API that may be useful for
  scripted oracle queries without writing a bridge from scratch.
- Status: evaluate, do not rely on yet. Treat as experimental.

### VBA / VBA-M / mGBA-NG forks
- Cite only when you need to explain historical behavior. **Not** an
  oracle. Older VBA in particular gets multiple things wrong.

---

## Hardware test ROMs (oracle ground truth)

### jsmolka/gba-tests
- Source: github.com/jsmolka/gba-tests.
- Use for: ARM and THUMB instruction-level tests, IRQ behavior,
  timer / DMA / waitstate behavior, memory mirrors. These are the
  closest thing to ground truth we have for the CPU and bus.
- Workflow: every passing test goes into the decoder/runtime
  validation matrix.

### nba-emu/hw-test (NanoBoyAdvance hardware tests)
- Source: github.com/nba-emu/hw-test.
- Use for: a second, broader hardware test suite. Some tests overlap
  with jsmolka; some are unique (notably more PPU edge cases).
- License: BSD-style, but we *run* these, we don't bundle the
  binaries unless their license explicitly allows it.

### endrift's mGBA test ROMs
- Source: github.com/mgba-emu/suite (mGBA test suite).
- Use for: PPU memory, BG modes, sprite priority, blending, window,
  affine math.

---

## GBA BIOS references

The BIOS is real, executed code in our runtime (see `PRINCIPLES.md`
"BIOS is sacred"). It is **not** redistributed in this repo — the
user provides their own dump at `bios/gba_bios.bin`. The references
below cover the BIOS structure, SWI entry points, and known
function boundaries so we can annotate behavior without lifting the
implementation.

### Hash identity

| Field    | Value                                                     |
|----------|-----------------------------------------------------------|
| SHA-1    | `300c20df6731a33952ded8c436f7f186d25d3492`                |
| MD5      | `a860e8c0b6d573d191e4ec7db1b1e4f6`                        |
| Size     | 16384 bytes (16 KB)                                       |

Any other hash → unknown variant. The loader refuses to start.

### Public BIOS disassemblies

These exist on the open web and are *symbol references only*. The
BIOS bytes themselves remain the authoritative behavior.

| Source                                  | What it gives us                                                                 |
|-----------------------------------------|----------------------------------------------------------------------------------|
| **GBATEK** § "GBA BIOS Functions"       | SWI numbers, calling conventions, semantics for each documented SWI.             |
| **Endrift's mGBA source**               | Functional reference for SWI behavior in C (DO NOT lift; cross-check only).      |
| **PineappleEA / Normmatt BIOS replacement** | Open-source HLE replacement; valuable as a *behavior reference*, NEVER the runner. |
| **Various community disassemblies** (search "gba bios disassembly") | Function boundaries, BIOS variable layout, IRQ dispatcher structure. |

### What we may pull from BIOS disassemblies

- Function boundaries (start address → end address).
- SWI table layout and per-SWI entry addresses.
- IRQ dispatcher entry/exit structure.
- Static variable locations the BIOS uses inside its IWRAM region.
- The BIOS's Nintendo-logo bytes (referenced for the cartridge logo
  check; we don't *copy* them, we just know the BIOS reads them).

### What we must NOT pull from BIOS disassemblies

- Decompiled / pseudo-C BIOS source as truth. Same rule as game
  decomps: bytes are the oracle, anything else is a hypothesis.
- HLE replacement implementations. Our interpreter runs the real
  bytes, period.
- BIOS bytes themselves into committed source.

### Where BIOS symbols land in this repo

- `bios/gba_bios.sym` — TSV when populated: `addr  mode  name`.
- `bios/gba_bios.toml` § `[[audit.regions]]` — code-vs-data carve-out
  for audit tools.
- Annotations cited inline in source code only where they explain a
  load-bearing decision (e.g., "the BIOS IRQ dispatcher reads the
  user handler from 0x03007FFC — see GBATEK § BIOS").

### Ghidra mandate (BIOS)

Per `ROADMAP.md` Phase 2.x: if the BIOS walk produces function
boundaries that disagree with the public disassemblies, or if a
specific SWI's entry signature can't be resolved from public
sources, **Ghidra is mandated** to disambiguate. Set up under
`bios/ghidra/` (gitignored) only when this trigger fires. We do not
preemptively stand up Ghidra; the public disassemblies cover most
needs.

---

## Game-specific references — read this section carefully

### zeldaret/tmc (Minish Cap decomp)
- Source: github.com/zeldaret/tmc.
- License: typically a non-commercial decomp license; **inspect
  before borrowing anything**.
- **What we may use:**
  - Symbol map (function names ↔ addresses).
  - Function boundary list.
  - ROM layout knowledge (sections, alignment).
  - Asset / data labels (what a given table is for).
  - Annotations that describe hardware-facing behavior ("this
    function programs DMA channel 3 for audio").
  - Comments that document what a ROM routine does, as a guide for
    cross-checking against our oracle.
- **What we must NOT use:**
  - Decompiled C as truth. The decomp's C is itself an artifact and
    not always semantically identical to the ROM.
  - Any PC-port runtime (renderer, audio mixer, input shim) the
    decomp's `tools/` or `port/` directory ships with.
  - Any HLE helper the decomp uses to "simulate" hardware.
- **How to import:** `MinishCapRecomp/tools/import_tmc_symbols/`
  reads the decomp's symbol output and writes a clean TSV into
  `MinishCapRecomp/symbols/imported_symbols.tsv` and
  `function_boundaries.tsv`. The original decomp source never
  enters our build.

### Other Minish Cap disassemblies / mods
- Treat the same way. Symbols and structure are fine; behavior is
  not.

### Ghidra mandate (Minish Cap)

Per `ROADMAP.md` Phase 5: if zeldaret/tmc symbol import leaves
unresolved dispatch-miss entries we can't seed manually, or if
indirect jump tables in compiled ROM code aren't visible to our
static analyzer, **Ghidra is mandated** for symbol extraction.
Set up under `MinishCapRecomp/ghidra/` (gitignored) only when this
trigger fires. Do not preemptively set it up.

---

## ROM identity

We pin specific ROM versions by SHA-1. Each supported version lives
in `MinishCapRecomp/config/<version>.toml`. The runner verifies the
hash on startup and refuses to launch with an unknown ROM.

We don't ship the ROM. We don't ask the user to dump theirs. The
build expects a local path the user provides.

---

## License posture

- All third-party source we link or compile lives in `third_party/`
  with its license intact and a one-line note in
  `third_party/README.md` saying what it is and how we use it.
- We never copy proprietary or non-permissively-licensed text into
  our own source. GBATEK and similar documents are referenced by
  citation, not by paste.
- Emulator code (mGBA, NanoBoyAdvance) lives in the **oracle**
  binary, not the native binary. The native runtime has no GPL/MPL
  emulator dependency.

---

## When references disagree

If GBATEK and CowBite disagree → check mGBA source. If mGBA and
NanoBoyAdvance disagree → write or find a hardware test ROM. If
no test exists → write one. Hardware test is the tiebreaker; do
not pick the more convenient interpretation.
