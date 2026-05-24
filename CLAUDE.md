# CLAUDE.md — Project Rules

---

# RULE SOURCE

All rules live in:
- `PRINCIPLES.md`
- `DEBUG.md`
- `TCP.md`

This file does not redefine rules.

---

# FAILURE MODE

If you:
- guess
- skip the first divergence
- trust unvalidated tool output
- propose a fix without tracing the writer / scheduler / decoder
- edit generated C to "fix" a symptom
- add HLE behavior to make a game work by accident
- stub a BIOS SWI, skip the BIOS intro, or otherwise bypass the
  real BIOS execution path
- **make any runtime exec path depend on the interpreter** (BIOS,
  cart, SWI fallback, "temporary scaffold," "until codegen catches
  up" — all forbidden; see PRINCIPLES.md "Interpreter is informative,
  never load-bearing")
- ship a "hybrid" runtime where some PCs are recompiled and others
  are interpreted

→ your response is INVALID
→ restart from `DEBUG.md`

---

# PROJECT OVERVIEW

Static GBA recompilation (ARM7TDMI / ARMv4T, ARM+THUMB interworking)
→ C/C++ → native.

Fixes belong in:
- the recompiler (`src/armv4t`, `tools/gba_recompile`)
- the runtime (`src/gba`, `src/runtime`, `src/debug`)
- per-game config (`MinishCapRecomp/game.toml`, symbols)

**Never** in `MinishCapRecomp/generated/`.

---

# BUILD LOOP

1. Build platform core (`gbarecomp/`).
2. Run `gba_recompile` over the ROM + config → fresh `generated/*.c`.
3. Build the game binary (`MinishCapRecomp/`).
4. Run (normal / verify / oracle-compare mode) **with BIOS path AND
   ROM path**. Both must hash-verify or the runtime refuses to start.
5. **Check `dispatch_misses.log`** next to the executable. If
   non-empty, add discovered functions to `game.toml`, regenerate,
   rebuild, re-run. Repeat until empty.

A dispatch miss is a silent game-breaking bug. Resolve all misses
before debugging anything else.

# BIOS RULE — RECOMPILED, NOT INTERPRETED

The GBA BIOS at `bios/gba_bios.bin` is **recompiled and executed
via the dispatch table**, not stubbed, not HLE'd, **not interpreted
on any runtime hot path**. SWIs and IRQs run through the recompiled
BIOS bytes via `runtime_dispatch`. There is no HLE path. There is
no interpreter fallback.

See `PRINCIPLES.md`:
- "BIOS is sacred — and recompiled, not interpreted (SHOWSTOPPER)"
- "Interpreter is informative, never load-bearing (SHOWSTOPPER)"

Every game's first render frames are BIOS frames, produced by
recompiled BIOS code. We do not fast-forward, stub, or interpret
through them.

If the runtime exec loop calls `Interpreter::step` for ANY PC, the
recompiler is broken. Fix the recompiler.

---

# PHASE 2.7 GATE — BIOS INTRO MUST BE FLAWLESS BEFORE ROM

No `--rom`, no `gba_recompile` toward game generation, no
`MinishCapRecomp` work, no Phase 5 — until the BIOS intro is
**visually + audibly + in-memory flawless** against the mGBA oracle.

Acceptance:
- `python oracle/diff_frame.py --scan 1 240 1` → IDENTICAL for
  OAM, PAL, VRAM, IWRAM at every frame.
- Per-frame framebuffer pixel-equality vs `emu_screenshot`.
- Audio sample-stream equality (or agreed perceptual tolerance)
  across the chime duration.
- `bios_intro_flawless` ctest green.

See `PRINCIPLES.md` "BIOS intro must be flawless before ROM" and
`docs/ROADMAP.md` Phase 2.7 for the work breakdown.

If you're tempted to "just try loading a ROM real quick" or
"check what happens with --rom" — DON'T. The gate exists because
shortcuts here propagate into invisible bugs everywhere else.

---

# DISPATCH MISS RULE

After EVERY game run — manual, scripted, or test — check
`dispatch_misses.log`. Format:

```
extra_func 0x08XXXXXX  thumb|arm
```

Add to `[functions]` in `game.toml`. Regenerate. Rebuild.

---

# FILES

Editable:
- `gbarecomp/src/**`, `gbarecomp/tools/**`, `gbarecomp/tests/**`
- `MinishCapRecomp/game.toml`
- `MinishCapRecomp/symbols/*`
- `MinishCapRecomp/src/main.cpp`, `MinishCapRecomp/src/game_config.*`

Never:
- `MinishCapRecomp/generated/*`

---

# TCP

Default native port: 19842. Oracle (mGBA bridge) on the next port.
Configurable per project via `debug.ini`.

See `TCP.md`.

---

# SYNC RULES

- Sync via **hardware events**, not raw frame numbers.
  Useful sync points: VBlank IRQ count, DMA completion count, timer
  overflow count, SWI count, BIOS-IRQ-return count, specific PC at
  specific function entry.
- Expect divergence early. The earliest divergence is the only one
  with a root cause; everything after is consequence.
- Debug **writes** to VRAM/OAM/PAL, not "what's on screen."

Priority order for early-boot debugging:
1. BIOS handoff / SWI behavior
2. Initial CPSR / mode / SP setup
3. IRQ vector + `IE`/`IME` configuration
4. DMA channel programming
5. Timer programming
6. DISPCNT / BGxCNT initial writes
7. Palette / VRAM / OAM initial population
