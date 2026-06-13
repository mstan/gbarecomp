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
- add load-bearing HLE behavior to make a game work by accident
  (the ONLY permitted HLE is a verified, opt-in, present-time shadow
  that reverts loudly and never becomes the verify oracle — see
  PRINCIPLES.md "Verified-enhancement HLE is permitted")
- stub a BIOS SWI, skip the BIOS intro, or otherwise bypass the
  real BIOS execution path
- let a dispatch miss *silently* interpret forever — no log, no
  self-heal to static, no miss record (see PRINCIPLES.md "Honest
  self-healing"); the interpreter may bridge a miss ONLY if it is
  loudly logged, recompiled on the fly, and fed back to the TOML
- call a build "statically recompiled / done" while PCs were
  interpreted or healed-from-cache this session, without reporting it
  (see PRINCIPLES.md "Coverage honesty is load-bearing")
- auto-write derived functions into `game.toml` instead of a reviewed
  proposal file merged by a human

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
5. **Check the coverage report + miss-list** next to the executable.
   The runtime self-heals misses at runtime (interpret → recompile on
   the fly) and logs them to `recomp_master_misses.toml.frag`. Review
   the generated `recomp_seed_proposals.toml`, **manually merge** the
   real ones into `game.toml`, regenerate, rebuild, re-run. Repeat
   until the coverage report reads FULLY STATIC (zero interpreted /
   healed-from-cache).

A dispatch miss no longer breaks the game (it self-heals), but the
build is NOT done until coverage is fully static. Never auto-write
`game.toml` — proposals are human-reviewed and merged.

# BIOS RULE — RECOMPILED AND DISPATCHED (SELF-HEALS IF MISSED)

The GBA BIOS at `bios/gba_bios.bin` is **recompiled and executed
via the dispatch table**, not stubbed, not HLE'd. SWIs and IRQs run
through the recompiled BIOS bytes via `runtime_dispatch`. There is no
hand-written HLE path. A missed BIOS PC **self-heals** (bridged,
recompiled on the fly, logged) — never silently HLE'd or stubbed; 100%
recompiled BIOS coverage stays the goal.

See `PRINCIPLES.md`:
- "BIOS is sacred — recompiled and dispatched (with honest self-healing)"
- "Honest self-healing — interpreter may bridge, must heal to static + report"
- "Coverage honesty is load-bearing"

Every game's first render frames are BIOS frames, produced by
recompiled BIOS code. We do not fast-forward or stub through them.

If a BIOS PC is interpreted, it must be loud, self-heal to native, and
seed the next build — never a silent, permanent interpreter path.

---

# PHASE 2.7 GATE — BIOS INTRO MUST BE FLAWLESS BEFORE ROM

**STATUS: ✅ CLOSED 2026-05-28 — the BIOS intro is complete.** It boots
visually + audibly + in-memory correct via `runtime_dispatch`
(recompiled BIOS), interpreter offline-only; Minish Cap boots through it
into gameplay. Cart/ROM work (FireRed and other games) is UNBLOCKED. The
open MC-HP-002 hang is downstream *game* execution, not a BIOS defect.
See `docs/ROADMAP.md` Phase 2.7. The gate below is satisfied — kept for
the record and because it re-arms if the BIOS intro ever regresses.

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

After EVERY game run — manual, scripted, or test — check the coverage
report (exit banner) and the miss-list `recomp_master_misses.toml.frag`.
Misses self-heal at runtime, but each is a discovery gap to close.

The miss→proposal writer emits `recomp_seed_proposals.toml` with
`[[extra_func]]` / `[[jump_table]]` entries:

```
[[extra_func]]
addr = 0x08XXXXXX
mode = "thumb"   # or "arm"
note = "proposed from miss-log ..."
```

**A human reviews and merges** the real ones into `game.toml` (never
an auto-write). Regenerate. Rebuild. Goal: coverage report = FULLY
STATIC.

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
