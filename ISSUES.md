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

### HP-002: Native-res "warble"/tear-like line, bottom-ish, high-action scenes — all games
- **Observed:** 2026-07-17 (user, during MC-WS-002 work). At the
  standard 3:2 resolution, high-action areas show a subtle warble
  described as "like a screen tear towards the bottom-ish" of the
  screen. More subtle than the (now fixed) MC-WS-002 margin seam;
  general graphics complaint across games. NOT the margin-camera
  seam (that was margin-only and is fixed in 9a63c6c/MinishCap).
- **Measured evidence so far (present-cadence probe, 257f599):**
  (a) The native/fixed-width present path requests NO vsync at all
  (`host_window.cpp` sets `SDL_RENDERER_PRESENTVSYNC` only for
  `resize_driven_view`) — unsynchronized D3D9 blit presents CAN tear
  at a semi-consistent scan phase because the FramePacer is periodic,
  which fits a "consistent bottom-ish tear line".
  (b) Runtime pacing hitch: ~7% of presents arrive >25 ms late,
  quasi-periodically every ~13–14 frames (~230 ms), 92% immediately
  followed by a ~8 ms catch-up present — a 4–5 Hz delivery
  oscillation measured in `_present_cadence.csv` (6180-present real
  run). Root cause unidentified (suspects: sidecar/provider fill,
  audio-bridge mutex, pacer sleep overshoot, game-thread codegen).
  (c) 59.7275-on-165 Hz pulldown (2/3-refresh dwell) exists whenever
  VRR is not pacing; secondary on the 60 Hz monitor (1 dup/~4.7 s).
- **Tools ready:** always-on per-present ring + summaries + CSV
  (`GBARECOMP_PRESENT_CADENCE=1`), `tools/analyze_present_cadence.py`;
  ChatGPT-consulted present architecture (SDL3/custom-D3D11 flip
  model + G-Sync windowed; emulation/presentation thread split; DWM
  `cRefresh` is NOT a scanout ruler — use flip-model
  `GetFrameStatistics`).
- **Next steps:** (1) verify the tear line's screen position
  consistency (user observation + probe run at native res);
  (2) root-cause the 230 ms hitch (instrument the game loop phases
  around a late present); (3) present-path architecture per the
  consult — flip-model vsync/VRR for every path (native included),
  FramePacer stays the emulation clock (MC-HP-004 rule).
- **Priority:** high — user-visible in normal play across all games.
- **▶ PROGRESS 2026-07-17 (Fable, branch fix/hp002-native-present) — two
  causes measured + fixed, artifact "reduced" twice by user, residual
  remains:**
  1. **Pacer 234 ms beat FIXED (`75bbcfa`).** New always-on frame-phase ring
     (`GBARECOMP_FRAME_PHASE` CSV + `tools/analyze_frame_phase.py`) attributed
     52/75 late frames (>25 ms) to the pacer's own sleep: `sleep_until`
     quantizes to the 64 Hz system tick; its drift against the 16.742 ms
     frame period beats at exactly 1/(1/15.625−1/16.742)=234 ms — the
     measured late-frame spacing. Guest spikes owned 15, `SDL_PollEvent`
     stalls 6 (one 496 ms), overlay compile 0. Fix: high-resolution waitable
     timer (Win10 1803+). After: late frames 4.2%→0.2% (all PollEvent),
     frame-total p95 = 16.755 ms.
  2. **Unsynchronized present FIXED (`d343959`).** Every path now requests
     vsync and Windows defaults to SDL's D3D11 flip-model backend (whole-frame
     flips, tearing structurally impossible; windowed VRR can engage).
     Verified: native path renderer=direct3d11 vsync=yes, fps 59.72 (pacer
     still the clock). Escape hatches: `GBARECOMP_NO_VSYNC=1`,
     `SDL_RENDER_DRIVER`.
  3. **Mid-frame scroll writes EXONERATED for walking** (new
     `GBARECOMP_MMIO_DUMP` flush of the always-on MMIO ring): 2264/2264 BG
     scroll writes in the walk replay land in VBlank — no content shear from
     scroll timing in that scenario. High-action scenes not yet traced.
  - **Residual after both fixes (user): "still there but reduced."** Prime
    remaining suspect: 59.7275-on-165 Hz pulldown (2.76 refreshes/frame dwell
    unevenness) whenever VRR is not actually engaged. Discriminating test:
    NVCP "Show indicator for G-SYNC" during windowed/fullscreen play, and/or
    temporarily set the panel to 60 Hz (59.73-on-59.94 ≈ one dup frame per
    ~4.7 s — near-perfect); if the artifact vanishes at 60 Hz, the fix is
    VRR engagement (or a refresh-matched present mode). Also still open:
    high-action MMIO trace (repeat item 3 in combat/effects scenes), and the
    emulation/presentation thread split for the PollEvent stall class.

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
- **[ROOT-CAUSED + FIXED 2026-07-02] WAITCNT was a red herring.** The fp
  cycle-trace (`build-interp` run twice, `FORCE_INTERP` 0/1, `INSN_TRACE`
  + `FP_SAVE`, `fp_pair_diff.py`) showed the two backends are **pc-identical
  and register-identical for all 7,362,482 instructions** of the boot — the
  ONLY difference is a single per-instruction cycle mismatch at
  **pc=0x00000188** (BIOS SWI epilogue `movs pc, lr`, `e1b0f00e`): recomp
  charges **0**, interpreter charges **3**. `access_cycles`/WAITCNT is shared
  and never varies, so it was not the cause.
  - **Mechanism (env-gated `cyc_probe` + `g_tick_ctx` tags in
    `runtime_bus_bridge.cpp` / `runtime_arm_default_aborts.cpp`):**
    `0x188` occurs 12× in boot; recomp charges 0 for all 12 (self-consistent);
    the interpreter charges 0 for 11 and **3 for one** — the return from
    **SWI 0x01 (RegisterRamReset)** at ROM `0x080b14e4`→`0x080b14e6`. That one
    SWI's handler is long (RAM clear, runs in System mode), so a
    VBlank-start **yield fires mid-handler**; the recomp resumes the handler
    **generated** (`[gen]`, exception-return charges 0) while force-interp
    resumes it **interpreted** (`[finterp]`, exception-return charges 3).
    The 3-cycle gap is the disagreement between the two cycle models for the
    `movs pc,lr` exception return.
  - **The recompiler was wrong, not the harness.** `emit_data_processing`'s
    exception-return path (`arm_codegen.cpp` ~L583) did
    `runtime_exception_return(r_var); return;` **without** first ticking the
    `_cyc` it had already computed as base+2 (the non-branch PC-write refill,
    via `writes_pc_nonbranch`). Its two sibling sites tick correctly — the
    normal PC-write path (~L652) and BOTH LDM exception-return paths
    (~L931/L996, the `ldmfd sp!,{...pc}^` IRQ-return idiom). So IRQ returns
    were charged right; only `movs/subs pc,lr` SWI returns dropped their
    2S+1N refill. Interpreter (the reference model), hardware (ARM7TDMI TRM:
    data-proc PC-write = 2S+1N), and the codegen's own computed `_cyc` all
    agree the cost is 3.
  - **Fix:** add `runtime_tick(cyc_var_for(ins))` before
    `runtime_exception_return` in the data-proc exception-return path
    (`arm_codegen.cpp emit_data_processing`), matching the LDM path.
    Recompiler-only; no generated file hand-edited.
- **Resolved:** 2026-07-02. Fix applied to `arm_codegen.cpp`; BIOS
  regenerated (`gba_recompile --bios bios/gba_bios.bin --config
  bios/gba_bios.toml` → diff = exactly 3 `runtime_tick` lines added at the
  BIOS exception-return sites 0x64/0x13C/0x188, nothing else). **Validated:**
  fp cycle-trace now shows **zero (cycle,pc) divergence over all 7,362,465
  aligned records** (was 1); cosim `clock` sub-hash now matches
  (`79a6c7…` both); L1 codegen diff harness 128/128; bus/interpreter/decoder
  smoke tests pass. `heal_gate_tests` fails to LINK (undefined core runtime
  symbols) — pre-existing target-linkage gap, unrelated. Superseded by LP-006
  (the next divergence the cosim now advances to).

### LP-006: recomp-vs-interp cpu-subhash split — stale `r8_12_user` bank (frame ~272)
- **Observed:** 2026-07-02, immediately after the LP-005 fix let the cosim
  run past cp1162. New FIRST DIVERGENCE at the SAME checkpoint (cp1162 /
  frame ~272 / cycle ~76.5M) — but now the **only** differing sub-hash is
  `cpu`; iwram/ewram/vram/pal/oam/io/audio/ppu/save/prefetch **and clock**
  all match.
- **Detail:** All architecturally-visible CPU state is byte-identical
  between backends (R0–R15, cpsr, and every `banked_sp/lr/spsr`). The split
  is in `r8_12_user` (the User/System r8–r12 shadow bank, hashed by
  `cosim_state.cpp hash_cpu` but not mode-visible): the cosim `cpu` dump
  (extended this session with `ur8..12`/`fr8..12`) shows
  **recomp `ur11=0 ur12=0` vs interp `ur11=0x1f ur12=0x9c3`**. `r8_12_fiq`
  matches. It surfaces at the RegisterRamReset (SWI 0x01) handler — the same
  mid-handler-yield → force-interp-interprets-the-BIOS path that exposed
  LP-005.
- **Analysis so far:** The interpreter's `MSR` mode-switch (`interpreter.cpp`
  ~L1081) only swaps `banked_sp/banked_lr` (R13/R14), never `r8_12_user`; and
  `write_user_reg` only writes `r8_12_user` in FIQ mode. So neither backend
  should touch `r8_12_user` during this non-FIQ (System↔SVC) handler — yet
  they diverge here (values matched through cp1161). Open question: which
  backend writes `r8_12_user` at this SWI, and which is correct. Likely the
  recomp's `bank_out/bank_in` (`runtime_arm.cpp`) updates the non-FIQ r8–12
  shadow where the interpreter leaves it stale (or vice versa).
- **Priority:** low. **Architecturally benign** — `r8_12_user` is invisible
  outside FIQ and no FIQ occurs in this boot; no functional effect, only the
  cosim full-state hash catches it. But it is the exact next first-divergence.
- **Root cause:** The recomp's `bank_out`/`bank_in` (`runtime_arm.cpp`) swap the
  R8..R12 FIQ/normal bank on **every** `old_bank != new_bank` transition (for
  non-FIQ moves this round-trips R8..R12 but writes `r8_12_user = R[8..12]`,
  keeping the shadow synced to the live normal bank). Its generated `msr` /
  `runtime_exception_return` therefore keep `r8_12_user` current across the
  handler's System↔SVC switches → 0/0. The **interpreter** only swapped
  R13/R14 at all five mode-switch sites (its acknowledged "we don't model the
  full banking matrix yet" gap), so force-interp left `r8_12_user` stale at the
  SWI-entry value (0x1f/0x9c3). The recomp is the complete side; the
  interpreter (reference model) was incomplete — completing it is the fix.
- **Resolved:** 2026-07-02. Added `swap_r8_12_banks()` in `interpreter.cpp`
  (mirrors `bank_out`/`bank_in`) and applied it at the interpreter's step-path
  mode-switch sites — `MSR`, `exception_return`, `restore_cpsr_from_spsr`.
  `enter_irq`/`enter_swi` (bios_smoke-only, not the cosim/self-heal path)
  deferred as a noted follow-up per constrain-surface-area. **Validated:** cosim
  recomp-vs-interp now runs **fully clean** through cp1280 / frame ~298 (max
  90M cycles), all 5 checkpoints identical chains, "no divergence within --max"
  (was FIRST DIVERGENCE at cp1162). L1 codegen diff harness 128/128;
  interpreter_smoke OK.
- **Diagnostics added this session (kept, env-gated / always-on):**
  `cyc_probe()` + `g_tick_ctx` tags in `runtime_bus_bridge.cpp` /
  `runtime_arm_default_aborts.cpp` (log every master-clock advance in a
  `GBARECOMP_CYC_LO..HI` window, tagged gen/finterp/bridge — the tool that
  cracked LP-005); and the `ur/fr` bank fields in the cosim `cpu` dump
  (`cosim.cpp`).

---

## Resolved

(none yet)
