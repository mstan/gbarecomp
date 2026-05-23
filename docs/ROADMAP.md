# Roadmap

Phases are intentionally narrow. Each one must be **measurably**
done before the next is started. "Looks like it boots" is not a
phase-completion criterion.

The roadmap pivoted in Phase 1 to make **BIOS execution** first-class
rather than a Phase-3 sub-bullet (see `PRINCIPLES.md` "BIOS is
sacred"). The current ordering reflects that:

- Phase 0: scaffold + repo skeleton
- Phase 1: ARM7TDMI / ARMv4T decoder + IR + interpreter
- Phase 2: BIOS bring-up (first render target = GBA BIOS intro)
- **Phase 2.7: BIOS intro flawlessness** (HARD GATE — must complete
  before Phase 3 or anything ROM-related)
- Phase 3: full GBA hardware runner (DMA, timers, IO, audio FIFO bookkeeping)
- Phase 4: oracle / debug workflow
- Phase 5: Minish Cap target (boots through the real BIOS)

> **HARD GATE** — Phase 2.7 is a stop sign. No ROM / cart / game work
> happens until the BIOS intro is **visually + audibly + in-memory
> flawless** against the mGBA oracle. See `PRINCIPLES.md` "BIOS intro
> must be flawless before ROM". The simplest piece of software the
> emulator runs is the foundation for everything else; if it isn't
> right, nothing built on top of it is trustworthy.

---

## Phase 0 — Discovery and repo skeleton  *(done)*

- [x] Both repos exist at `F:\Projects\gbarecomp\{gbarecomp,MinishCapRecomp}`.
- [x] CMake projects for both.
- [x] Project rule docs: `CLAUDE.md`, `PRINCIPLES.md`, `DEBUG.md`,
      `TCP.md`.
- [x] Architecture / reference / debugging / roadmap docs.
- [x] No-stubs / no-HLE policy written down.
- [x] Reference inventory written down.

---

## Phase 1 — ARM7TDMI / ARMv4T decoder + IR + interpreter

- [x] ARM decoder covering data processing (imm + shifted reg),
      multiply / multiply-long, single & halfword data transfer,
      block data transfer, branch / BX, SWI, swap, PSR transfer,
      undefined-instruction trap.
- [x] THUMB decoder covering all 19 formats relevant to GBA games.
- [x] Single CPU model with interworking; `BX` switches state.
- [x] PC-visible pipeline (+8 ARM, +4 THUMB).
- [x] Shifter carry-out into CPSR.C.
- [x] LDM/STM common case (S-bit handling still TODO).
- [x] SWI entry surfaces to the caller.
- [x] IR interpreter executes decoded instructions against a portable
      `Bus` interface.
- [x] Interpreter smoke covers DP, branches, BL link, PUSH/POP,
      LDR/STR, MUL, THUMB↔ARM interworking.
- [ ] Multiply-long execution (UMULL/SMULL/UMLAL/SMLAL) in the
      interpreter — decoded, not yet executed.
- [ ] MLA execution in the interpreter — decoded, not yet executed.
- [ ] LDM/STM `^` (S-bit: user-mode bank, SPSR restore).
- [ ] MRS/MSR field-mask handling (operand decode is stubbed).
- [ ] SWP execution.
- [ ] Conformance pass against jsmolka/gba-tests via the interpreter.

The remaining Phase 1 items will be force-fixed by Phase 2 — the
BIOS exercises essentially all of them, and any divergence between
our interpreter and mGBA will surface immediately.

---

## Phase 2 — BIOS bring-up  *(current)*

The deliverable is **rendering the GBA BIOS intro screen.** This is
our first render target. It exercises the full boot path: BIOS in
memory, interpreter executing, IRQ scheduler running, PPU advancing
scanlines, timers ticking, DMA copying tile/palette data, a host
window presenting frames.

Acceptance: launch `gbarecomp_bios_smoke` (and later the game runner
with no cartridge) and the Nintendo logo intro plays on screen at
real speed, matching mGBA's BIOS output frame-for-frame.

### 2.1 BIOS load + identity

- [x] `bios/gba_bios.bin` in-tree path (gitignored binary).
- [x] `bios/gba_bios.toml` audit config.
- [ ] BIOS loader reads + SHA-1-verifies the image; refuses to start
      on hash mismatch.
- [ ] CLI flag `--bios <path>` on every game-runnable executable.

### 2.2 Real memory bus

- [ ] BIOS region (0x00000000..0x00003FFF) backed by the loaded image.
- [ ] EWRAM (256 KB at 0x02000000) backed by a real array, mirrored.
- [ ] IWRAM (32 KB at 0x03000000) backed and mirrored.
- [ ] IO page (0x04000000..0x040003FF) routed to per-register
      handlers; unknown IO logs structured, never returns magic.
- [ ] PAL (1 KB at 0x05000000) backed, mirrored.
- [ ] VRAM (96 KB at 0x06000000) backed, with the 32 KB obj-tile
      mirror fold.
- [ ] OAM (1 KB at 0x07000000) backed, mirrored.
- [ ] Cartridge ROM (0x08000000+) reads from the loaded ROM bytes.
- [ ] Open-bus on unmapped reads; structured log on any unmapped
      write.

### 2.3 IRQ scheduler (minimal)

- [ ] `IE`, `IF`, `IME` IO registers behave per GBATEK.
- [ ] CPU IRQ entry: CPSR → SPSR_irq, mode → IRQ, LR_irq → PC+4,
      PC → 0x00000018, I bit set.
- [ ] CPU IRQ return path (SUBS PC, LR, #4 / LDMFD ^).
- [ ] VBlank IRQ raised by PPU when transitioning to scanline 160.

### 2.4 PPU (minimal)

- [ ] DISPCNT (BG mode select, OBJ enable, forced blank, etc.).
- [ ] DISPSTAT (VBlank/HBlank/VCount flags + comparator + IRQ enables).
- [ ] VCOUNT increments per scanline.
- [ ] BG modes used by the BIOS intro (likely tile mode 0 with
      affine BG3 — confirm via mGBA trace).
- [ ] Sprite compositing from OAM (the Nintendo logo is rendered
      via BG tile data, not OBJs — confirm).
- [ ] Per-scanline framebuffer output (240×160 RGB).

### 2.5 DMA channels (minimal)

- [ ] DMA0..3 immediate mode (memory-to-memory copy).
- [ ] DMA0..3 VBlank-start mode (the BIOS uses this for palette
      and tile copies).
- [ ] No-op on the audio FIFO modes for now; sound is silent during
      Phase 2.

### 2.6 Timers (minimal)

- [ ] TIMER0..3 counter + reload + prescaler.
- [ ] Timer overflow → IRQ pending (the BIOS uses one timer for the
      intro animation cadence; verify with mGBA).
- [ ] Cascade mode (TIMER N+1 increments on N overflow).

### 2.7 Host window + main loop

- [ ] SDL2 (or platform-native) 240×160 framebuffer presenter,
      integer-scaled.
- [ ] Main loop: run interpreter for ~16.78 MHz / 60 cycles per
      frame slice → tick PPU/timers/DMA → present → handle input.

### 2.8 Sync to oracle (informal)

- [ ] At least one stop point in the BIOS intro where we can
      manually compare our register state to mGBA's via its scripting
      hooks. (Formal TCP oracle bridge is Phase 4.)

**Ghidra mandate point** (Phase 2.x): if the BIOS walk produces
function boundaries that disagree with the public disassemblies
(`docs/GBA_REFERENCE_NOTES.md` § "GBA BIOS references"), or if a
specific SWI's entry signature can't be resolved from public sources,
**Ghidra is mandated** to disambiguate. We will not guess. Set up
under `bios/ghidra/` (gitignored) only when this trigger fires.

---

## Phase 2.7 — BIOS intro flawlessness  *(HARD GATE)*

This phase is a stop sign between BIOS bring-up and everything that
comes after. The deliverable is a flawless GBA BIOS intro on three
axes — visual, audible, in-memory. See `PRINCIPLES.md` "BIOS intro
must be flawless before ROM."

### Acceptance criteria (all three must pass)

1. **In-memory.** `python oracle/diff_frame.py --scan 1 240 1`
   reports **IDENTICAL** for OAM, PAL, VRAM, IWRAM at every PPU
   frame from boot through the post-intro idle state.
2. **Visual.** A per-frame framebuffer diff between our PPU's
   rendered output and `emu_screenshot` from the mGBA oracle shows
   0 pixel-level differences for every PPU frame in the same range.
3. **Audible.** The native audio output sample stream matches the
   oracle's `emu_audio_samples` byte-for-byte (or within an agreed
   perceptual tolerance — TBD) across the chime duration.

### Work breakdown

#### 2.7.A Reset alignment

mGBA's `GBAReset` pre-bakes SP banks and leaves the CPU in System
mode. Our reset starts in SVC mode at PC=0 per ARM ARM A2.6.5. Pick
one convention and apply it on both sides so per-instruction
lockstep (`oracle/find_first_diverge.py`) can start from instruction
1. Recommended: keep native correct-to-hardware, drive mGBA's reset
state to match via `writeRegister` after `core->reset(core)`.

#### 2.7.B Memory drift → zero

Using `diff_frame.py`, walk every PPU frame through the intro and
identify each byte that differs. For each: root-cause in the
recompiler infrastructure (interpreter, bus, IO, cycle accounting,
IRQ entry/return). No symptom patches. Until this is green, no
audio / framebuffer work — they will mask the underlying memory
issues.

#### 2.7.C Audio subsystem

`src/gba/gba_audio.cpp` is a stub. Stand it up:
- SOUND1..4 (DMG square/wave/noise) channels per GBATEK § "GBA Sound
  Channels 1-4."
- SOUNDA/SOUNDB FIFO sound channels driven by DMA1/2 + timer
  overflow per GBATEK § "GBA Sound Channels A,B (DMA Sound)."
- 32 kHz mixer output sufficient for the BIOS chime.

#### 2.7.D Audio diff

Extend the oracle with `emu_audio_samples` (read mGBA's mixed
output). Extend native to expose its own `audio_samples`. The diff
harness compares sample streams over the intro window.

#### 2.7.E Framebuffer diff

Use the existing `emu_screenshot` (oracle) and native `ppu.render()`
output. Wire both into the diff harness with a per-frame
framebuffer-equality assertion. Identify each pixel-level mismatch
and fix at the source (PPU compositing path).

#### 2.7.F Regression test

`bios_intro_flawless` ctest case spins up native + oracle, drives
both through the full intro, asserts memory + framebuffer + audio
equality at every PPU frame. CI green = gate closed; CI red = gate
re-opens automatically.

### What's explicitly NOT allowed during Phase 2.7

- No `--rom` work on `bios_smoke`.
- No cartridge / cart header / save chip work.
- No `gba_recompile` improvements toward generating game code.
- No `MinishCapRecomp/` work.
- No "let me just check one thing on a ROM" detours.

When the gate is closed, Phase 3 unblocks.

---

## Phase 3 — Full GBA hardware runner

Phase 2 lands the minimum bus + PPU + IRQ + DMA + timers needed to
run the BIOS. Phase 3 fills in everything the BIOS doesn't exercise
that real games will need.

- [ ] DMA HBlank-start mode + video-capture (DMA3) + sound-FIFO
      (DMA1/2 from timer 0/1 overflow).
- [ ] Timer cascade + audio-driving modes.
- [ ] PPU: full BG modes 0..5, affine BG, all window/blend/mosaic
      paths, sprite priority, sprite affine.
- [ ] Input: KEYINPUT, KEYCNT.
- [ ] Save chip detection (SRAM/Flash 512K/1M/EEPROM 4K/64K) and
      read/write state machines.
- [ ] Audio FIFO bookkeeping (sound is allowed to be silent on
      output for now — but FIFO/timer/DMA flow must be correct,
      because games key game-logic timing off audio DMA).
- [ ] Audit subset of test ROMs (jsmolka, nba-emu hw-test) passing
      against the live runtime.

---

## Phase 4 — Oracle / debug workflow

- [ ] mGBA bridge process as a separate target with its own TCP listener.
- [ ] Always-on frame ring buffer in the native runtime.
- [ ] `rdb_*` per-store / per-block / per-call rings under
      `GBARECOMP_REVERSE_DEBUG`.
- [ ] `framebuf_diff`, `frame_diff`, `memory_diff`, `io_diff`,
      `first_failure` commands end-to-end.
- [ ] Deterministic input scripts (`playthrough/*.json`) replayable
      against both native and oracle.

---

## Phase 5 — Minish Cap target

Strict ordering. Don't jump milestones. **Every milestone requires
booting through the real BIOS first.** No milestone is met by
shortcutting the BIOS.

1. [ ] ROM hash verified; BIOS hash verified.
2. [ ] Header / entrypoint decoded.
3. [ ] BIOS validates the cartridge logo and jumps to entry; first
      cartridge instruction at `0x08000000` executes.
4. [ ] First executed game function identified by PC.
5. [ ] First VBlank / IRQ path matches oracle (with real BIOS
      dispatch).
6. [ ] First meaningful VRAM / OAM / PAL writes match oracle.
7. [ ] Title screen renders.
8. [ ] Input reaches game code; menu navigation works.
9. [ ] New-game path starts.
10. [ ] Save type detected, save / load round-trips against oracle.

**Ghidra mandate point** (Phase 5): if zeldaret/tmc symbol import
leaves unresolved dispatch-miss entries we can't seed manually, or
if indirect jump tables in compiled ROM code aren't visible to our
static analyzer, **Ghidra is mandated** for symbol extraction.
Set up under `MinishCapRecomp/ghidra/` (gitignored) only when this
trigger fires. Do not preemptively set up Ghidra.

---

## What stays out of scope (for now)

- 3DS / ARM9 / ARMv5T extensions.
- Enhancement features (widescreen, internal-resolution scaling,
  60→fps speedups, frame interpolation).
- Save-state portability across builds.
- Networking.

---

## When Ghidra is *not* mandated

To keep the principle clean: we do **not** stand Ghidra up
preemptively. It's a heavyweight tool and the symbol surface we
have access to (BIOS disassemblies + zeldaret/tmc) covers most
needs. Stand it up only at one of the explicit trigger points above.
Document the trigger that fired in the commit / handoff where the
Ghidra project gets created.
