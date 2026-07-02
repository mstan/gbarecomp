# Known Issues

Issues that are known, tracked, and intentionally deferred. Entries
are ordered by severity, not chronology. New issues go in the
section that matches their impact; closed issues stay (with a
"Resolved" line) for the historical record.

The bar for adding here: something the user observed, the team
reproduced, and we chose not to fix immediately. Not a TODO list.

---

## High priority

### HP-001: Phase 2.7 pass invalidated by interpreter-not-load-bearing rule
- **Observed:** 2026-05-23. The Phase 2.7 acceptance pass earlier in
  the day relied on `armv4t::Interpreter::step` driving execution.
  Subsequently the user clarified that the interpreter is forbidden
  on any runtime hot path (PRINCIPLES.md "Interpreter is
  informative, never load-bearing (SHOWSTOPPER)"). The prior pass
  therefore does NOT satisfy the new gate.
- **Detail:** Acceptance criteria for Phase 2.7 (memory equality,
  framebuffer equality, audio sample-stream equality vs mGBA) all
  green'd against an interpreter-driven `bios_smoke`. The
  recompiled runtime (`MinishCapRecomp.exe`, future game-runners)
  must reproduce those results with `runtime_dispatch` as the sole
  execution engine before the gate re-closes.
- **Current state:** `MinishCapRecomp.exe` aborts on first
  instruction with `runtime_arm: dispatch miss for pc=0x00000000`
  because (a) the BIOS isn't recompiled yet and (b) per-IrOp
  codegen isn't written. That abort IS the open gate.
- **Priority:** high — blocks Phase 5 and any game-runner work.
- **Next step:** Phase 2.8 (per-IrOp ARM + THUMB codegen, BIOS
  recompilation tooling, runtime dispatch wire-up) followed by the
  Phase 2.7 re-pass via recompiled execution. See `docs/ROADMAP.md`
  Phase 2.8.A–E.

---

## Low priority

### LP-001: BIOS intro boot tempo plays slightly fast
- **Observed:** 2026-05-23, end of Phase 2.7. User listened to the
  BIOS chime via `bios_smoke --window` after the SDL audio rate fix
  (`2d2fa17 host_window: open SDL audio at 65536 Hz (BIOS rate)`)
  and reported the chime structure is correct but plays "a bit
  fast" — visibly the GAME BOY wordmark also seems to advance a
  touch quicker than mGBA.
- **Measurement:** `diff_audio.py` over 240 PPU frames reports
  identical sample counts (260,330) and identical effective rate
  (65,536 Hz) on both sides. RMS error 5.11 across 260K samples
  (~0.016%); 12 sample-level diffs. The DIFFERENCE in samples
  is tiny; the perceived "fast" cannot be explained by the buffer
  contents.
- **Suspected cause:** PPU frame pacing or SDL audio device's
  effective playback rate negotiating to something other than the
  requested 65,536 Hz on this host. Not a mixer bug.
- **Priority:** low. The BIOS intro is byte-identical to mGBA on
  framebuffer / OAM / PAL / VRAM / IWRAM at every frame 1-200;
  audio buffer contents are 99.984% identical. The remaining
  tempo difference does not propagate any incorrect state to cart
  code; Phase 5 (Minish Cap) work will exercise the timing paths
  harder and either expose or contextualize the cause.
- **Next step when picked up:** confirm `got.freq` from the SDL
  device matches `want.freq = 65536`; if not, either resample or
  pace the queue using `got.freq`. Also profile frame-pacing for
  any sub-1% drift in the main loop.

### LP-002: 12 audio sample diffs vs mGBA on BIOS chime
- **Observed:** 2026-05-23, `diff_audio.py --frames 240` over the
  BIOS intro.
- **Detail:** 12 of 260,330 samples diverge (0.0046%). Pattern is
  "native one mix-sample ahead of mGBA at FIFO transitions" for 7
  events, plus a 5-sample run in the post-chime tail. Max
  `|native - oracle|` = 2,112 on int16 scale. RMS 5.11.
- **Suspected cause:** sub-cycle timing interaction between the
  audio sample-event scheduler and timer overflow events that
  drive the FIFO drain. The formulas match mGBA's; the
  interleaving may not in edge cases.
- **Priority:** low. See LP-001 — within perceptual tolerance and
  the BIOS path is the simplest workload the audio mixer sees.
  Phase 5 cart code will exercise the FIFO more thoroughly and
  likely surface a more legible reproduction.
- **Next step when picked up:** instrument
  `GbaAudio::fifo_timer_step` with a per-call log capturing
  `cycle_accumulator_`, `until_cycles`, `start`, and the
  sample-event scheduler's queue position; compare to mGBA's
  `mTimingUntil` at the corresponding cycle.

### LP-003: `bios_intro_flawless` ctest not yet written
- **Observed:** Phase 2.7.F item from `docs/ROADMAP.md`.
- **Detail:** The regression-locking test that asserts byte
  equality of BIOS intro framebuffer + memory + audio (subject to
  LP-001 / LP-002 tolerances) against mGBA exists only as the
  manual harness `diff_frame.py` + `diff_audio.py`. CI cannot
  catch a regression in the BIOS path until this lands.
- **Priority:** low (only because no further BIOS work is planned
  near-term; if BIOS changes resume, this graduates to high).
- **Next step when picked up:** Wrap `diff_frame.py --scan 1 30 1`
  in a ctest invocation that spawns native + oracle, asserts no
  region diff beyond the documented BG0HOFS open-bus byte. Add an
  audio assertion that the sample buffer matches to within LP-002
  tolerances.

### LP-004: [RESOLVED 2026-07-02] open-bus prefetch latch differs across backends during HALT
- **Observed:** 2026-07-01, by the recomp-vs-interp first-divergence
  co-simulation oracle (`COSIM_ORACLE.md`).
- **Detail:** After the harness-fidelity fix (`be01a1b`: force-interp
  data accesses routed through the runtime bus bridge), the recomp and
  force-interp backends are **per-instruction bit-identical (cycle +
  PC) for all 799,688 instructions** of a 7-frame MinishCap boot trace.
  The cosim then halts on ONE residual: during the BIOS
  `IntrWait`/HALT loop (BIOS pc ~0x348, frame 6+, `halted=1`), the
  **open-bus prefetch sub-hash differs**. Every other subsystem is
  identical at that checkpoint — CPU/CPSR, IWRAM, EWRAM, VRAM, PAL,
  OAM, IO (IE/IF/IME/timers/DMA/WAITCNT), audio, PPU, and the master
  clock all match. The difference is **persistent** (frozen while
  halted; does not self-heal, unlike the DMA-steal transient that
  `be01a1b` removed). It is the Game-Pak/BIOS open-bus latch
  (`gba_bus.cpp` `bios_prefetch_` / `latch_bios_prefetch`): the two
  backends latched a different prefetch word at the last BIOS
  instruction before halting.
- **Impact:** benign in effect — dead state UNLESS the guest reads the
  protected-BIOS region or unmapped memory (open bus) while this value
  is latched (the one load-bearing class: see
  `project_mc_hp_002_animation_reframe`). No incorrect open-bus read
  observed here, so not safe to dismiss but not observed to matter.
- **Priority:** low. Cannot be adjudicated recomp-vs-interp (both are
  ours); resolving "which backend is right" needs the independent
  cycle-accurate oracle (NanoBoyAdvance :19844 / mGBA :19843) — the
  `COSIM_ORACLE.md` §8 escalation, not yet built.
- **Next step when picked up:** (1) add raw `bios_open_bus()` to the
  cosim `dev` dump; capture both backends' latched value + last
  pre-halt PC. (2) Adjudicate vs NBA/mGBA — whichever matches the
  hardware open-bus value at that PC is correct. (3) Fix the wrong
  side's latch timing in the recompiler/runtime (never a stub or a
  cosim-hash exclusion — the latch is real state, cf. MC-HP-002).
- **CONFIRMED BIOS-LEVEL / game-independent (2026-07-02):** FireRed
  reproduces the class with a BYTE-IDENTICAL signature — at the same
  cp30 / frame 6 / `halted=1`, FireRed's `cpu` (434a8bb4…), `io`
  (e7e682bb…), and BOTH prefetch values (A=057f…, B=6fa6…) exactly
  match MinishCap's; only `iwram`/`save` differ between the games.
  Both titles boot through the same recompiled BIOS into the identical
  `IntrWait`/HALT state, so the split is in the shared BIOS prefetch
  path, not in any game's code. Impact upgraded accordingly: it is not
  MinishCap-specific — one BIOS-path fix resolves it for every game.
  Still low priority (benign in effect) but now clearly worth a proper
  fix. Gate 1 (recomp-vs-recomp) passed on FireRed (305 cp, 0 diverge)
  with RTC pinned via `RECOMP_RTC_EPOCH`.
- **Resolved:** 2026-07-02, commit `c6a9df5`. Raw `prefetch_raw` values
  (added to the cosim `dev` dump) + BIOS disassembly showed the CPU
  halted at PC=0x348: recomp latched `prefetch_word(0x348)` =
  `BIOS[0x350]` = `e8bd4010` (the correct ARM PC+8 fetch); force-interp
  froze at `BIOS[0x34c]` = `0afffffc` (PC+4, one instruction behind).
  **The recomp was correct** — this was a harness (force-interp) bug,
  NOT a shipped-recomp bug. Cause: the recomp's contiguous generated
  block stream runs the *next* block's prologue (which latches the
  post-halt PC) before the yield-check bails on `halted`; the
  force-interp path returns to `step_once`, whose halt branch
  intercepts first and skips that latch. Fix (harness-only, shipped
  code untouched): the force-interp driver re-latches the BIOS prefetch
  for the new PC when it just halted, mirroring the recomp's
  fall-through prologue. Verified: MinishCap recomp-vs-interp clean
  frame 6 → frame 272 (~44× further). BIOS-level, so it clears FireRed
  too. Superseded by LP-005 (the next divergence).

### LP-005: recomp-vs-interp 3-cycle skew under WAITCNT prefetch (frame ~272)
- **Observed:** 2026-07-02, cosim recomp-vs-interp after the LP-004 fix.
- **Detail:** At cp1162 / frame ~272 the two backends diverge with a
  genuine **3-cycle clock skew** (A cyc 76536923 vs B 76536926; the
  `clock`, `cpu`, `audio`, `ppu` sub-hashes all split; prefetch / io /
  memory match). The `dev` dump shows **`waitcnt 0x4014`** — the Game
  Pak **prefetch buffer** (bit 0x4000) + non-zero waitstates are
  enabled — with DMA3 active (`d3_src 03007ee0 d3_dst 02000030`).
- **Suspected class:** WAITCNT / Game-Pak-prefetch cycle-timing
  (burndown axis-2, the "+42-cycle boot skew / WAITCNT remains" item).
  Note `gba_bus.cpp access_cycles` currently returns **hardcoded
  "Default WAITCNT 0x0000" ROM costs and ignores the WAITCNT register +
  the prefetch-enable bit** — so ROM-access timing is unmodeled when
  the game programs WAITCNT. That is a *shared* helper (would mis-time
  both backends equally), so the recomp-vs-interp *skew* is likely a
  second-order difference in when a prefetch/branch cost is charged
  under WAITCNT; the fp cycle trace will name the exact instruction.
- **Priority:** low→medium (first genuine cycle-timing divergence not
  yet root-caused; likely the real axis-2 lead).
- **Next step when picked up:** capture both fp rings near frame 273
  (`GBARECOMP_INSN_TRACE=1` + `GBARECOMP_FP_SAVE`; ring holds ~7–8
  frames so run to ~273), `fp_pair_diff.py` to name the first
  cycle-charge mismatch, then reconcile — and separately, model WAITCNT
  in `access_cycles` (both backends) since it is currently unmodeled.

---

## Resolved

(none yet)
