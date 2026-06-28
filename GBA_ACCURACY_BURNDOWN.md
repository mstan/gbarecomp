# GBA Accuracy Burndown (living doc)

Companion scorecard for the faithful GBA recompiler core, modeled 1:1 on the
psxrecomp `ACCURACY_BURNDOWN.md` 7-axis framework
(`F:\Projects\psxrecomp\_wt-tomba2\psxrecomp\ACCURACY_BURNDOWN.md`). That project
owns the methodology; this doc transposes it to ARM7TDMI / GBA hardware and
tracks the burndown across **all seven accuracy axes** for the shared engine that
drives MinishCap + the four Gen3 Pokémon recomps.

Worktree: `accuracy/gba-burndown` (isolated from active development).
Engine HEAD at fork: `3262c49`.

---

## ⚠️ LESSON (carried from PSX, binding here)

- A research-claimed discrepancy is a **HYPOTHESIS, not a bug**. Validate the
  OUTPUT (diff our result vs the oracle on the SAME input) BEFORE changing code.
  "Matches GBATEK / matches NBA source conventions" does NOT justify rewriting
  code whose output is already oracle-correct.
- Self-agreement (compiled == our interpreter) proves **backend-equivalence, NOT
  correctness** — both can be identically wrong. This is project CLAUDE.md /
  PRINCIPLES.md doctrine, not optional.
- The user's eyes / the oracle's OUTPUT override source-reading. "Looked right
  before, wrong after" = revert, always.

## Method (non-negotiable — the dual gate)

Every item gets: **status**, the **external comparative(s)** to cross-reference
it against, and a **validation method**. "Looks good" is NOT a status —

> **An item is only GREEN once it is BOTH (a) cross-referenced against a
> reference (GBATEK / NanoBoyAdvance source / mGBA source / a hardware test ROM)
> AND (b) runtime-validated against an accurate oracle. Self-agreement
> (compiled == our interp) is necessary but NOT sufficient.**

Status taxonomy (per item): `[ ]` open · `[~]` partial/in-progress · `[x]`
done-and-dual-gate-validated. Per-axis verbal status: `STRONG` /
`MODERATE` / `PARTIAL` / `WEAK` / `NOT-MODELED`.

Granularity verdicts (the GBA cycle vocabulary): `CYCLE-ACCURATE` /
`SCANLINE-ACCURATE` / `FRAME-ACCURATE` / `EVENT-SCHEDULED` / `NOT-MODELED`.

Always-on ring buffers only — **never arm-then-capture**. Probes QUERY a window
of a continuously-recording ring; they do not arm recording at probe time, and
they never pause/step two emulators into lockstep to "synchronize observers"
(project CLAUDE.md ring-buffer rule).

## Comparative sources (the reference shelf)

- **GBATEK** (nocash) — the canonical GBA hardware reference. Cite the section
  per item. (Replaces psx-spx.)
- **NanoBoyAdvance source** (GPL-3.0, isolated oracle binary) — the
  **cycle-accurate reference**: first public GBA emu to pass the AGS aging-cart
  tests; models the Game-Pak prefetch buffer, waitstate timing, open-bus, and a
  scheduler with absolute master-cycle timestamps (`scheduler.GetTimestampNow()`).
  Read for the exact timing model; run as the oracle for cycle + audio.
  (Replaces in-tree Beetle.)
- **mGBA source + oracle** (MPL-2.0, in-tree at `oracle/`, port 19843) —
  very-accurate, already integrated with an injection bridge (`emu_write`,
  `emu_set_regs`, `emu_set_keys`) and a post-resample audio tap
  (`emu_audio_samples`) + per-instruction master clock (`mTimingCurrentTime`).
  The **state/divergence oracle**; the interim audio oracle until the NBA pre-
  resample tap lands. NOT cycle-accurate for bus/prefetch/PPU.
- **SkyEmu** (MIT) — fallback cycle-accurate-CPU oracle with a built-in HTTP
  control server + per-sample APU; keep in reserve.
- **Hardware test ROMs** (ground truth above emulators): mGBA suite, AGS aging
  tests, gba-tests (jsmolka), PeterLemon/GBA. Running these on native vs NBA is
  the strongest single validation we can add — **build the harness (below).**
- **In-tree interpreter** (`src/armv4t/interpreter.cpp`) — oracle for the
  *compiled backend only* (backend-equivalence; necessary, not sufficient).

## Validation infrastructure to BUILD (prerequisite tooling)

- [x] **NanoBoyAdvance oracle binary** — built `_nba_oracle/nba_oracle.exe`
  (GPL-isolated; minimal CMake superproject links only the `nba` core, no
  SDL/Qt). Hooks (i) `scheduler.GetTimestampNow()` guest-cycle export and (ii)
  an always-on **pre-resample** mixer sample ring at `APU::StepMixer()`
  (apu.cc:185, raw int16 L/R + cycle stamp); **HLE MP2K hack disabled**
  (`mp2k_hle_enable=false`) so the guest's real M4A mixer runs. Line-JSON TCP on
  :19844. *Boot rate 32768→65536 Hz at SOUNDBIAS raise; cycle stamps confirm
  256 cyc/sample.*
- [x] **Always-on, non-destructive recomp audio capture ring** — the existing
  `ring_` (`gba_audio.cpp:459`) is a consume-once playback FIFO drained by BOTH
  SDL and the `audio_samples` TCP cmd (they steal each other's samples) — a
  ring-buffer-discipline violation. Added a parallel always-on capture ring
  (`CapSample` × 262144) keyed by `samples_generated_`, queried by window,
  with a per-channel tap (ch1-4 + direct_a/b) for the PSG bit-check. `audio_cap`
  TCP cmd. Verified non-destructive; per-channel isolation works.
- [x] **Drift-tolerant audio comparator** — `oracle/diff_audio_drift.py`:
  cross-correlation lag alignment, post-align Pearson + normalised-RMSE,
  onset-timing histogram, per-note pitch-cents error; plus an isolated PSG
  per-channel bit-check mode. Self-test passes (lag 0, r 1.0). First run vs NBA
  below.
- [ ] **Native↔NBA cycle first-divergence comparator** — `cyc_watch`-style
  always-on block-leader cycle sampler on the recomp + `GetTimestampNow()` on
  NBA; diff per guest-PC anchor by deltas (offset-cancelled). The backbone for
  axes 2/3. (Recomp already has `g_runtime_cycles` + the insn-fp ring; needs the
  NBA side + an anchor-query command.)
- [ ] **Hardware-test-ROM harness** — run jsmolka gba-tests + mGBA suite on
  native vs NBA, diff pass/fail + result registers. Highest-leverage axis-1/2
  validator.
- [ ] **State-surface diffs (have, extend)** — `oracle/diff_iwram.py` (phase-
  aligned full IWRAM/EWRAM), `oracle/diff_frame.py` (OAM/PAL/VRAM/framebuffer)
  already exist; extend to NBA.

---

## Axis 1 — Instruction semantics (ARM7TDMI / ARMv4T)

**Status: STRONG** — full ARM + THUMB, interpreter-cross-validated; not a timing axis.

- [x] ARM + THUMB decode → shared IR — GBATEK ARM opcode tables; decoders
  `src/armv4t/arm_decode.cpp` (391 L), `thumb_decode.cpp` (549 L), IR
  `arm_ir.h:66-94`. Covers ALU (AND…MVN), BL/BX interworking, LDR/STR/H/SB/SH,
  LDM/STM, SWP/SWPB, MUL/MLA/UMULL/UMLAL/SMULL/SMLAL, MRS/MSR, SWI.
- [x] Reference executor (flags, shifter carry, banked regs/modes) —
  `interpreter.cpp` (1134 L). Validated against the codegen via the **L1 diff
  harness** `tests/codegen/` (interp vs generated fn, diff R0-14 + CPSR + memory
  + branch side-effects; 1137 L of cases).
- [ ] **Exhaustive opcode-space proof** — cross-ref jsmolka `gba-tests/arm` &
  `thumb` + mGBA suite; validate native-vs-NBA on result registers. Current
  proof is bounded by the hand-written case set — **the gate-(b) gap.**
- [ ] Edge cases to pin vs HW ROM: `LDM/STM` base-in-list & empty-list, `R15`
  read offset (+12 vs +8), `MSR` flag-vs-control byte masks, multiply timing
  flags, `SWP` bus locking.

**Gap:** no hardware-test-ROM run yet (gate (b) unmet for the long tail).
**Lever:** stand up the test-ROM harness against NBA.

## Axis 2 — Cycle / timing  ← ACTIVE

**Status: PARTIAL → cycle-accurate vs the in-tree interpreter, NOT yet vs HW.**

- [x] Master guest clock `g_runtime_cycles`, advanced every recompiled
  instruction — `runtime_bus_bridge.cpp:564`. Per-instruction cost = fixed base
  (`instr_cycle_base`, `arm_ir.cpp`) + memory N/S cycles + multiply operand
  waits; codegen emits `runtime_mem_cycles()` / `runtime_mul_cycles()`
  (`arm_codegen.cpp:789,918,984,1075`).
- [x] Region/bus-width-aware memory cost — `GbaBus::access_cycles`
  `gba_bus.cpp:433-466` (IWRAM/OAM/IO=1, VRAM/PAL 16-bit, EWRAM +2, ROM
  N16=5/S16=3/N32=8/S32=6). Branch pipeline refill +2 folded into base.
- [ ] **GAP 1 — dynamic WAITCNT.** Waitstates hardcoded to power-on default
  0x0000 (`gba_bus.cpp:422,455`). Games that reprogram WAITCNT (most do — sets
  ROM to 3/1 + prefetch) diverge in cycle counts. Cross-ref GBATEK §"WAITCNT";
  validate native-vs-NBA Δcycles over a WAITCNT-sensitive region.
- [ ] **GAP 2 — no prefetch-buffer (pipeline) timing.** `prefetch_word`
  (`gba_bus.cpp:83`) synthesizes open-bus values only, not ROM sequential-
  prefetch *timing*. NBA models this; we don't. Cross-ref GBATEK §"GamePak
  Prefetch"; this is the single biggest HW-cycle divergence source.
- [ ] **Validation method (the ruler):** port the PSX `cyc_watch` pattern —
  always-on block-leader cycle sampler, arm ONE guest-PC anchor on recomp + read
  `GetTimestampNow()` on NBA, free-run both, diff per `hit_index` by **deltas**
  (consecutive same-anchor hits cancel the boot offset). NO pause/step.

**Gap:** WAITCNT + prefetch ⇒ cannot be true-GREEN vs hardware yet.
**Lever:** implement dynamic WAITCNT + a prefetch-timing model, gate vs NBA Δcyc.

## Axis 3 — Interrupt / event timing

**Status: MODERATE** — event-scheduled with lazy device catch-up; no true
absolute-timestamp ordered queue.

- [x] Min-horizon event budget — `recompute_event_budget` = min(PPU dot, timer,
  audio-sample) `runtime_bus_bridge.cpp:522-530`; `runtime_tick` debits
  `g_event_budget`, materializes `tick_devices` at the horizon `:554-575`.
- [x] IRQ sources — VBlank/HBlank/VCount `:498-506`, timer overflow
  (`tick_timers:463`), timed DMA (`run_timed_dma:491,496`); taken when
  `irq_pending && I-clear` `:577`. HALT wake latency modeled
  (`kIrqWakeDelayCycles` `:578-607`). Vectoring `runtime_irq`
  (`runtime_arm.cpp`): banked LR/SPSR, drive-to-completion nesting
  (`g_irq_iret_depth`).
- [x] **Always-on cycle-stamped IRQ-vector log** — `runtime_arm.cpp:383-423`,
  131072 entries, one per vector + `from_halt` flag. (Discipline-correct ring.)
- [ ] **GAP — intra-chunk ordering is fixed code order** (VBlank→HBlank→VCount→
  DMA), not per-cycle timestamp ordering. Invisible to fingerprint diffs because
  MMIO reads get exact catch-up (`runtime_mmio_catch_up:538`), but it is not a
  real ordered queue. Cross-ref GBATEK IRQ priority + NBA scheduler; validate IRQ
  raise-cycle vs NBA `GetTimestampNow()` at the raise site.
- [ ] IRQ **take-point precision** — HW takes at an exact instruction boundary;
  we take at a block/horizon edge. Quantify the slip vs NBA.

**Gap:** event ordering & take-point are edge-quantized, not cycle-exact.
**Lever:** absolute-timestamp event queue + raise-cycle diff vs NBA.

## Axis 4 — Memory / MMIO

**Status: STRONG for side-effects + open-bus; DMA timing is the gap.**

- [x] IO read/write side-effects — `gba_io.cpp` (585 L); MMIO access forces
  `runtime_mmio_catch_up` + `runtime_resync_horizon`
  (`runtime_bus_bridge.cpp:372-425`).
- [x] **Faithful open-bus** — `Region::OpenBus` synthesizes the prefetched
  opcode (`open_bus_pc_`/`open_bus_thumb_` latch) `gba_bus.cpp:147-149,194-196,
  248-250`; BIOS protected-region reads return the BIOS prefetch latch
  `:114,164,215`. (This was the MC-HP-002 root-cause fix.) Cross-ref GBATEK
  §"Unpredictable Things" / open-bus.
- [x] DMA engine — immediate `run_immediate_dma` `gba_io.cpp:106`, VBlank/HBlank
  `run_timed_dma:190`, sound-FIFO `run_sound_fifo_dma:246` (FIFO-A/B timer-select
  `:306-315`).
- [ ] **GAP — DMA cycle-stealing not modeled.** Transfer loop
  `gba_io.cpp:212-225` moves all words instantaneously and charges **zero**
  cycles to `g_runtime_cycles` — no CPU stall. Cross-ref GBATEK §"DMA Transfer
  Time"; validate vs NBA Δcyc across a large DMA. DMA3 video-capture (mode 3)
  also not modeled (`:188-190`).
- [ ] Full MMIO read-to-clear / write-1-ack / open-bus-on-unmapped-IO audit vs
  GBATEK register table; validate via an MMIO write-trace tally vs NBA.

**Gap:** DMA steals no cycles ⇒ timing wrong during heavy DMA.
**Lever:** charge DMA cycles + CPU-stall model; diff vs NBA.

## Axis 5 — Peripherals / video (+ audio pointer)

**Status: PARTIAL** — PPU SCANLINE-ACCURATE; bitmap modes + mosaic missing.

- [x] PPU dot-by-dot advance (`GbaPpu::tick` `gba_ppu.cpp:77-110`); line rendered
  once at its HBlank (`render_scanline` `:473`). Compositor `:337-754`: BG modes
  0/1/2 (regular + affine), OBJ regular + rot/scale + double-size, WIN0/WIN1/OBJ-
  window, alpha blend + BLDY brightness, exact OBJ/BG priority `:626-634`.
- [ ] **GAP — not per-pixel:** mid-scanline MMIO writes (mid-line scroll/BGCNT)
  use end-of-line register state. Cross-ref GBATEK; validate vs NBA per-scanline
  capture on a raster-effect ROM.
- [ ] **Mosaic not implemented**; **bitmap modes 3/4/5 not rendered** (only
  0/1/2 in `:583-595`). Cross-ref GBATEK §"LCD VRAM Bitmap Modes".
- [ ] DMA3 video capture, green-swap, forced-blank edge cases.
- → **Audio is Axis-5's largest sub-surface; broken out below as the audio-first
  plan.** Wired at `tick_devices:462` (`bus.audio().tick`).

**Gap:** sub-scanline raster effects, bitmap modes, mosaic.
**Lever:** per-pixel (or mid-line resync) render path; bitmap modes; pixel diff
vs NBA.

## Axis 6 — Static-vs-dynamic recompiler fidelity (recompiler-unique)

**Status: STRONG** — static-first with honest, measured self-heal.

- [x] Coverage metric `FULLY_STATIC` / `NOT_STATIC` + counters (distinct misses,
  interpreted insns, healed-to-native) — `self_heal.h:82-94`; machine-readable
  JSON every exit; proposal frag `recomp_master_misses_<tag>.toml.frag`
  (`runtime.cpp:1120-1135`).
- [x] Dispatch miss → interpreter bridge (`runtime_dispatch_miss`), logged,
  healed to native overlay (Stage 2), seeds reviewed TOML — never silent, never
  auto-merged (`self_heal.h:1-16`). Honest-self-healing doctrine enforced by
  CLAUDE.md.
- [x] Gen-code validated by the L1 codegen diff harness (Axis 1) + always-on
  fingerprint-ring cycle diffs.
- [ ] **GAP — "fully static" is a per-run tally; correctness of gen code is
  bounded by the codegen test set, not exhaustive** (the gate-(b) caveat again).
  Mid-function aliasing / resume-PC seeds are recent (memory:
  project_midfunction_aliasing) — pin vs interpreter on resume entries.
- [ ] Self-modifying / dynamic-RAM code path (deferred-until-stable per
  feedback_constrain_surface_area) — out of scope until the static common case
  is GREEN.

**Gap:** exhaustive gen-code correctness proof.
**Lever:** test-ROM harness + per-run coverage must read FULLY_STATIC with zero
heals before any axis is called GREEN for a given game.

## Axis 7 — Determinism

**Status: STRONG except one wall-clock leak.**

- [x] Core is cycle-driven off `g_runtime_cycles`; no `rand`/wall-clock in
  CPU/PPU/DMA/timer/audio paths (`chrono` in runtime is profiling-only, gated by
  `g_phase_prof`). Audio sampling cycle-driven (`gba_audio.cpp` `tick`).
- [x] Savestates reproducible — version-locked snapshot (`debug/snapshot.cpp`);
  fp-ring + cycle clock re-origin on load (`runtime.cpp:1213-1219`).
- [ ] **GAP — RTC reads host time:** `std::time(nullptr)` + `localtime_r`
  (`gba_rtc.cpp:325-327`, `load_datetime`/`load_time` `:230-256`). Run-to-run
  nondeterministic for RTC-using games (RSE / FRLG). Cross-ref: must be seedable
  to a fixed epoch for deterministic oracle runs.
- [ ] Audio savestate gap — SOUND3/SOUND4/wave RAM intentionally NOT serialized
  (`gba_audio.cpp:41-46,81-85`); re-syncs in ~1 frame but a state-load starts
  them cleared (1-frame divergence right after load — matters for state-loaded
  oracle runs).

**Gap:** RTC host-time leak; post-load audio re-seed glitch.
**Lever:** injectable fixed RTC epoch (oracle mode) + serialize PSG ch3/4.

---

## 7-Axis Status Table (verdict · gap · lever)

| # | Axis | Verdict (granularity) | Dual-gate | Biggest gap | Lever |
|---|------|----------------------|-----------|-------------|-------|
| 1 | Instruction semantics | STRONG (full ARM+THUMB) | (a)✔ (b)✖ HW-ROM | No exhaustive opcode proof | Test-ROM harness vs NBA |
| 2 | Cycle / timing | PARTIAL (cycle-acc vs interp only) | (a)✔ (b)✖ | WAITCNT pinned + no prefetch timing | Dynamic WAITCNT + prefetch model, Δcyc vs NBA |
| 3 | Interrupt / event | MODERATE (event-scheduled) | (a)✔ (b)~ | Fixed-order, edge-quantized take-point | Absolute-timestamp queue; raise-cyc diff vs NBA |
| 4 | Memory / MMIO | STRONG (open-bus) / DMA gap | (a)✔ (b)~ | DMA steals 0 cycles | Charge DMA cycles + stall; diff vs NBA |
| 5 | Peripherals / video | PARTIAL (scanline-accurate) | (a)✔ (b)~ | Sub-scanline FX, bitmap modes, mosaic | Mid-line resync + bitmap modes; pixel diff |
| 5a| **Audio (sub-axis)** | **PARTIAL → chime drift-GREEN vs NBA** | (a)✔ (b)~ chime done | mono; loudness ratio 0.021; PSG bit-check pending | **Audio-first plan ↓** (first results in) |
| 6 | Static/dynamic fidelity | STRONG | (a)✔ (b)~ | Gen-code proof bounded by case set | Test-ROM + FULLY_STATIC gate per game |
| 7 | Determinism | STRONG (one leak) | (a)✔ (b)~ | RTC host-time; PSG state-load reseed | Injectable RTC epoch; serialize ch3/4 |

GREEN count (pre-burndown baseline): **0/7 fully GREEN** — every axis failed
gate-(b) until NBA was wired. **Post-burndown (MinishCap, 2026-06-28):** axes 1,
6, 7 GREEN; axis 2 near-GREEN (boot transient, shrunk by the DMA fix); axis 3
faithful (NBA fake-SIO artifact ruled out); axis 5 faithful (capture-phase
artifact ruled out); axis 4 (DMA cycle-stealing) FIXED + validated. Remaining
real work: the test-ROM harness (axis-1 gate-(b)), WAITCNT/prefetch (axis-2 boot),
per-game audio/PSG-balance, and cross-game validation. Two of the three flagged
"drifts" were oracle/measurement artifacts — the recomp tracks the cycle-accurate
oracle far more closely than the raw sweep first implied.

---

## Audio-first plan (Axis 5a) — the first runnable oracle slice

GBA audio = 4 PSG channels (SOUND1 square+sweep, SOUND2 square, SOUND3 wave,
SOUND4 noise) + 2 Direct-Sound PCM FIFO channels (A/B) fed by timer-clocked DMA,
driven by the guest's real M4A/MP2K software mixer. **Our APU is a host model
(`GbaAudio`), not recompiled guest code** — the guest runs the real MP2K driver
and writes FIFOs; we translate IO writes + FIFO words into samples
(`gba_audio.cpp`). Sample generation is **cycle-deterministic** (1024-cycle
audio event, 32768 Hz default / 65536 Hz post-BIOS-SOUNDBIAS), so PSG output is
bit-reproducible given identical IO writes — making the PSG bit-check tractable.
Direct-Sound output is data-dependent on guest mixer math → compare
drift-tolerantly.

**Oracle:** NanoBoyAdvance (cycle-accurate), pre-resample mixer ring at
`APU::StepMixer()`, HLE MP2K hack DISABLED so the guest's real mixer runs.
Interim: mGBA `emu_audio_samples` (post-resample) until the NBA tap lands — the
metric harness is oracle-agnostic.

**Fidelity bar (user-chosen): drift-tolerant on the mixed stream + near-bit-exact
on isolated PSG channels.**

Mixed-stream metric (drift-tolerant — cross-emulator resample/phase makes
bit-exact unrealistic):
1. **Cross-correlation lag alignment** — find best integer sample lag maximizing
   normalized correlation; report lag + peak coefficient.
2. **Post-alignment RMS error + Pearson correlation** over the aligned overlap.
3. **Onset-timing histogram** — energy-envelope onset detection on both streams;
   histogram of per-onset time deltas (catches tempo/trigger drift).
4. **Per-note pitch error** — short-time autocorrelation pitch track on both;
   per-frame cents error distribution (catches frequency/sweep errors).

PSG channel metric (near-bit-exact):
5. **Isolated per-channel capture** (ch1-4) from the new `audio_cap` ring →
   exact-sample compare recomp-vs-recomp-interp (backend-equivalence) and, where
   the oracle can mute FIFO, recomp-vs-NBA. Report first divergent sample index +
   max abs error.

**Ring-buffer discipline:** capture is an **always-on, non-destructive ring**
keyed by `samples_generated_`; the probe QUERIES a window `[start_idx, count]`.
It does NOT drain the playback FIFO and does NOT pause/step the emulators.

### Audio slice status

- [x] **Recomp always-on capture ring + `audio_cap` TCP cmd** — mixed + per-
  channel (ch1-4 + direct_a/b), keyed by `samples_generated_`, window-queried,
  **non-destructive** (separate from the playback FIFO `ring_`). Fixes the
  drain-once discipline violation. `src/gba/gba_audio.{h,cpp}` (CapSample /
  query_capture / cap_push) + `src/debug/tcp_debug_server.cpp` (`cmd_audio_cap`).
- [x] **NanoBoyAdvance oracle** — `_nba_oracle/nba_oracle.exe` (GPL-isolated):
  always-on pre-resample mixer ring tapped at `APU::StepMixer()` (apu.cc:185,
  raw int16 L/R + `GetTimestampNow()` cycle stamp), `mp2k_hle_enable=false`
  (guest's real M4A mixer runs), line-JSON TCP on :19844
  (`ping`/`reset`/`run_frames`/`audio_cap`/`quit`).
- [x] **`oracle/diff_audio_drift.py`** — metrics 1-5. Self-test (stream vs
  itself) = lag 0, r 1.0000, RMSE 0.0000, onsets matched. ASCII output.
- [x] **First comparison — recomp vs NanoBoyAdvance (cycle-accurate), BIOS
  chime, mixed-stream drift.** ↓ FIRST RESULTS.

#### FIRST RESULTS (2026-06-28) — recomp BIOS chime vs NanoBoyAdvance

150 frames from cold boot, capture-ring window from index 0, 65536 Hz:

| Metric | Result | Reading |
|--------|--------|---------|
| [1] xcorr lag | **−1 sample (−0.02 ms)** | essentially phase-locked |
| [1]/[2] Pearson r | **0.9906** | waveform shape matches the cycle-accurate oracle |
| [2] normalised-RMSE | 0.137 | small residual after amplitude-normalise |
| [3] onset timing | 8/8 matched, mean **+0.62 ms**, max **4.99 ms** | trigger timing within ~5 ms |
| [4] pitch error | 30 voiced frames, mean **+0.4 c**, max **5.9 c** | pitch within ~6 cents |
| loudness ratio (nba/recomp) | **0.021** | ← the calibration gap (see below) |

**Verdict:** the recompiler's BIOS-chime audio is **drift-GREEN against a
cycle-accurate oracle** (gate-(b) satisfied for the chime): correlation 0.99,
sub-sample lag, ≤5 ms onset, ≤6 cent pitch. This is the first dual-gate
data point on Axis 5a.

**Actionable finding — loudness.** recomp peaks at int16 ~19200 vs NBA raw
mixer ~400 (different output domains by construction: our `kGain=600` / direct
×48 int16 scaling vs NBA's pre-`/0x200` mixer units). Drift metrics normalise
this out, but the **0.021 ratio is exactly the loudness knob** flagged at
`gba_audio.cpp:836/856/922`. Next: derive the domain map from the measured
ratio and calibrate so relative *and* absolute levels track the oracle.

- [x] **In-game M4A music** (beyond the BIOS chime) — MinishCap intro music
  (frames ~80-200, sustained Direct-Sound M4A) vs NBA: **r=0.9646**, near-identical
  activity (71361 vs 71355 nonzero), onset within one detector hop. Faithful —
  slightly looser than the chime (0.99) as expected for the timing-sensitive
  software mixer + cross-domain compare. No clear defect.
- [ ] **PSG per-channel bit-check** — **blocked: MinishCap is Direct-Sound-only**
  (ch1-4 captured ZERO across frames 60-800; confirmed via the per-channel ring).
  The bit-check needs a PSG-driven scene/game (e.g. a Pokémon battle/title where
  M4A routes melody to PSG square/wave). Infra is ready (audio_cap exposes ch1-4);
  needs the right capture target. Note the per-channel tap is validated on
  Direct-Sound (chime: direct_a/b nonzero, ch1-4 zero, consistent).
- [ ] **Audio metric polyphony** — the autocorrelation pitch tracker is unreliable
  on polyphonic M4A (std blows up; median stays valid). For the PSG slice, isolate
  channels (already captured) rather than track pitch on the mix.
- [ ] **PSG-active ROM captures** (per game) — title/menu themes exercise PSG;
  run the per-channel bit-check. Slice target: MC + FR/LG + R/S + Emerald.
- [ ] **Loudness calibration** — `kGain=600` (`gba_audio.cpp:836`), direct ×48
  (`:856`), shadow `kShadowOutputScale=24000` (`:922`) are untuned knobs; tune
  to the oracle RMS once aligned. (memory: project_audio_psg_channels — "loudness
  oracle-tuning is the open follow-up".)
- [ ] **Mono → stereo** — current mix collapses routing to "either side audible"
  (`gba_audio.cpp:807-813`); HW is stereo. Defer until mono drift is GREEN.

---

## Instrumentation matrix (measure in PARALLEL, fix SERIALLY)

Measurement is additive and non-behavioral (NULL-guarded taps, always-on rings,
read-only peeks, comparators) — so all of it is built in parallel NOW. Whatever
comes back with drift enters the **serial burndown queue** below; behavioral
model changes are applied one at a time, each re-measured before the next.

Each axis needs the SAME signal exposed on both sides (recomp :19842 and the NBA
oracle :19844), then a comparator that aligns by a hardware anchor (PC / cycle /
VCount / IRQ-count — never raw frame number) and reports first-divergence + drift.

| Axis | Signal | Recomp side (:19842) | NBA oracle side (:19844) | Comparator |
|------|--------|----------------------|--------------------------|------------|
| 1 Instruction | opcode coverage + result regs | decoder inventory + `registers`; jsmolka test-ROM run | `registers`; test-ROM run | `build_instruction_coverage.py`; test-ROM result diff |
| 2 Cycle | Δcycles over a PC-anchored region | `g_runtime_cycles` + `cyc_anchor` ✓ | `GetTimestampNow` + `cyc_anchor`/`run_to_pc` | `diff_cycle_nba.py` (consecutive-anchor Δ, offset-cancelled) |
| 3 IRQ/event | raise-cycle + take-cycle per source | IRQ-vector ring + `irq_cap` ✓ | IRQ raise ring + `irq_cap` | `diff_irq_nba.py` (sequence + cycle Δ) |
| 4 Memory/MMIO | write trace {cyc,addr,val,size,pc} | `mmio_cap` ring ✓ | `mmio_cap` ring | `diff_mmio_nba.py` (tally + first-divergence) |
| 5 Video | per-scanline FB + VRAM/PAL/OAM | framebuffer + `read_*` ✓ | `screenshot` + `read` | `diff_video_nba.py` (pixel + surface diff) |
| 5a Audio | mixed + per-channel sample ring | `audio_cap` ✓ | `audio_cap` ✓ | `diff_audio_drift.py` ✓ **(done — chime r=0.99)** |
| 6 Static/dyn | coverage + heal tally | self-heal JSON / `state_hash` ✓ | n/a (oracle is reference) | per-run FULLY_STATIC gate |
| 7 Determinism | end-state hash, run-twice | `state_hash` ✓ | deterministic by construction | `determinism_probe.py` (recomp×2 + RTC check) |

Substrate status: audio (5a) DONE. **Recomp-side query commands `cyc_anchor`
(2), `irq_cap` (3), `mmio_cap` (4), `state_hash` (6/7) ADDED + substrate-
validated on `bios_smoke` (2026-06-28)** — read-only, always-on rings, non-
behavioral. `mmio_cap` + `state_hash` return live data under the interpreter
(addr/value/size + region hashes); `cyc_anchor` + `irq_cap` query recomp-runtime
rings (g_fp / g_irq_log) populated by generated code, so they return live data in
the per-game recomp build (empty under the interp oracle, which mirrors into its
own local rings). NBA oracle peers + `diff_*_nba.py` comparators still TO BUILD.
Axis-5 video already exposed (`screenshot`/`read_*`/`ppu_state`); a VCount-stamped
per-scanline ring is the only video gap. Coverage builder (1/6) queued. Pilot
game = MinishCap (one game rebuild carries all instrumentation into gameplay).

## Cross-axis sweep RESULTS (2026-06-28, MinishCap, recomp :19842 vs NBA :19844)

First full recomp-vs-NanoBoyAdvance sweep. Comparators self-proven (NBA-vs-NBA =
zero drift on every axis). MinishCap boots fully native (0 interpreted, 0
dispatch misses). Findings:

| Axis | Comparator | Result | Verdict |
|------|-----------|--------|---------|
| 1 Instruction | `build_instruction_coverage.py` | 44 IrOps, **42 impl, 0 decoded-not-impl**; only `BLX_reg` missing (ARMv5+, N/A on ARM7TDMI) | GREEN (static); HW-ROM run still owed for gate-(b) |
| 2 Cycle | `diff_cycle_nba.py` | steady per-frame Δ **bit-identical (280896=280896, +0)**; **+42 cyc** in the first boot-region interval only | NEAR-GREEN — tiny boot skew = WAITCNT/prefetch |
| 3 IRQ | `diff_irq_nba.py` | VBlank cadence **identical**; NBA fires ~13 Serial IRQ/frame, recomp 0 → **ROOT-CAUSED: NBA artifact, recomp faithful** (see Axis-3 resolution) | **RESOLVED** |
| 4 MMIO/DMA | `diff_mmio_nba.py` | DMA cycle-stealing **FIXED** (was: +40 FIFO writes from 0-cycle DMA); intra-frame skew +42→+2. `0x04000410` BIOS write = separate harmless item | **RESOLVED (DMA)** |
| 5 Video | `diff_video_nba.py` | At matched dynamic regs → **pixel-identical** (frames 25/30/128 = 0 diff); apparent 12695-px drift = capture-phase fade-ramp artifact → **RESOLVED: compositor faithful** (see Axis-5 resolution) | **RESOLVED** |
| 5a Audio | `diff_audio_drift.py` | chime r=0.9906, lag −0.02 ms, onset ≤5 ms, pitch ≤6 c; **loudness ratio 0.021** | drift-GREEN (chime); loudness knob |
| 6 Static/dyn | coverage banner | **fully native, 0 interpreted, 0 misses** (NOT_STATIC = 2 warm-cache boot shards only) | GREEN |
| 7 Determinism | `state_hash` cold×2 | recomp **byte-identical** (all regions); NBA also deterministic; MinishCap has no RTC | GREEN (RTC leak latent for RSE/FRLG) |

Notably, 3 axes already track the cycle-accurate oracle exactly (2 steady-state,
6, 7) and 1 is near-exact (2 boot). The real drift is concentrated in axes 3/4/5.

## Axis-3 resolution (2026-06-28) — Serial IRQ: NBA artifact, recomp faithful

First serial-burndown item. The comparator flagged NBA firing ~13 Serial
IRQ/frame vs recomp's 0. Disciplined investigation (a divergence is a HYPOTHESIS):

- **Register state identical both sides:** IE=0x0081 (VBlank+Serial enabled),
  IME=1, SIOCNT=0x6003→0x5088, RCNT=0. Not a state divergence.
- **recomp models no SIO IRQ** (`request_irq` never raises `IrqSerial`;
  source-confirmed) — looked like a recomp gap.
- **But the only SIOCNT start-bit write is `0x5088`** (start=1, **bit0=0 =
  EXTERNAL/slave clock**, 32-bit, IRQ-enable). GBATEK: a slave-clock transfer
  with no master never receives clock edges → never completes → start stays set
  → **no IRQ**. mGBA explicitly fixed "normal-mode transfers with no clock must
  not finish"; NBA still fake-completes any started transfer (ignores bit0).
- **Verdict: recomp is FAITHFUL (single-player, no link cable → no Serial IRQ);
  NBA's ~13/frame Serial IRQs are the emulator "fake SIO completion" artifact.**
  The drift is the ORACLE's inaccuracy, not the recomp's. (Validated: GBATEK +
  NBA source `bus/io.cc:610` + the live write trace.)

Action taken — added the FAITHFUL internal-clock SIO transfer model
(`gba_io.cpp` SIOCNT handler + `tick_sio` + budget wiring; GBATEK cycle table,
mirrors NBA's timing): an **internal**-clock start-bit edge schedules a
transfer-complete event that clears start, returns open-bus SIODATA, and raises
`IrqSerial` if SIOCNT.14. This closes the real gap for games that use
internal-clock SIO-IRQ, and is **provably inert for MinishCap** (which only does
external-clock slave SIO → never arms; determinism cold×2 identical
`bd9ad2bf…`; state byte-identical). NOT oracle-validated yet (no available game
/ test-ROM exercises internal-clock SIO-IRQ, and NBA's SIO is unfaithful so it
can't validate it) — cross-ref-GREEN, oracle-validation pending a suitable ROM.

Comparator follow-up — **DONE**: `diff_irq_nba.py` now annotates NBA's fake-SIO
Serial IRQs as a known oracle artifact and excludes them from the verdict
(precisely when recomp has 0 Serial; `--strict` to include), and flags timing
DRIFT only on steady-state (median) divergence so the axis-2 boot transient is
annotated, not failed. Axis-3 sweep now reads **ZERO-DRIFT**.

## Axis-5 resolution (2026-06-28) — PPU compositor: faithful; drift was capture-phase

Second serial-burndown item. Comparator flagged "VRAM/PAL/OAM byte-identical, yet
~12695 px differ" — looked like a clear compositor bug. Disciplined isolation:

- **Scanned frames for FULL state-sync.** When the screen is static the
  framebuffer is **pixel-identical** (frames 25/30/128 = 0/38400). The big diffs
  (frames 35–65, up to 12020 px) all coincide with an active cross-fade +
  growing PAL/OAM diffs → animation desync, not compositing.
- **Cleanest synced frame (10):** VRAM/PAL/OAM identical, DISPCNT/BGCNT/WIN all
  match; the ONLY differing compositor input is **`BLDALPHA` (recomp 0x0e02 vs
  NBA 0x0d03)** and the 682 differing pixels are subtle alpha-blends
  ((255,223,255) vs (255,239,255)).
- **Root cause:** the game writes a `BLDALPHA` cross-fade ramp **once per frame
  at scanline 160 (VBlank), pc=0x1c60**; it steps every frame (0f01→0e02→0d03→…).
  recomp's frame-step boundary and NBA's `run_frames` boundary capture the
  framebuffer **one ramp-step apart**, so the two captured frames use adjacent
  fade coefficients → the blended region differs. NOT a compositor math error.
- **Proof of faithfulness:** at frame 25, once the fade settles and `BLDALPHA`
  matches (0x0010 both), the framebuffer is **byte-identical** to the
  cycle-accurate oracle. The compositor (incl. alpha blend) is correct;
  identical inputs → identical pixels.
- (The MOSAIC/BLDY/WIN0/1 register "DIFFs" are reads of **write-only** registers
  → open-bus garbage on both sides; not real state.)

**Verdict:** compositor is FAITHFUL. The "drift" is a video-comparison
phase-alignment artifact (same lesson as `diff_iwram.py`'s VBlank-phase rule).
No recomp behavioral change. Methodology fix below.

Comparator follow-up (measurement, non-behavioral) — **DONE**: `diff_video_nba.py`
now phase-aligns. It reconstructs the *written* PPU control registers
(DISPCNT/BGxCNT/scroll/affine/WINx/MOSAIC/BLDCNT/BLDALPHA/BLDY) from the
`mmio_cap` write stream (`nba_common.mmio_shadow` / `ppu_input_parity`) — not
write-only read-backs — and renders a compositor verdict only at a frame where
surfaces AND those inputs match on both sides. `--scan LO HI` finds the first
input-aligned frame. Verdicts: COMPOSITOR-FAITHFUL (aligned + pixel-identical),
COMPOSITOR-BUG (aligned + pixels differ — the real signal), or NOT-PHASE-ALIGNED
(inputs differ; pixel diff is not a compositor verdict). MinishCap frame 6 →
**COMPOSITOR-FAITHFUL**.

NOTE — two of the three "live drift" axes (3 IRQ, 5 Video) root-caused to
oracle/measurement artifacts, not recomp bugs. The recomp is tracking the
cycle-accurate oracle far more closely than the raw comparators first suggested.
Remaining genuine items: axis-4 FIFO write-rate (+40/30fr, real, audio-DMA
cadence) and the axis-5a audio loudness knob.

## Axis-4 investigation (2026-06-28) — FIFO rate = DMA cycle-stealing (REAL gap)

Third item, and the **first genuinely real recomp gap** (3 and 5 were artifacts).
Live root-cause of the +40 FIFO_A/B writes/30 fr:

- Sound config identical both sides (SOUNDCNT_H 0x210e, DMA1/2 CNT_H 0xb600,
  SOUNDBIAS 0x4200).
- **FIFO_A write cadence is the tell:** recomp writes its 4-word sound-DMA burst
  at **interval 0** (all 4 words same cycle — instantaneous DMA); NBA at
  **interval 2** (~2 cyc/word — cycle-accurate). Burst period recomp 20064 vs
  NBA 20058 cyc; the ~6-cyc/burst gap is exactly the DMA transfer time NBA
  charges and recomp doesn't, accumulating to the FIFO-count delta.
- **Root cause (confirmed = recon gap):** the recomp DMA loop
  (`gba_io.cpp` run_immediate_dma / run_timed_dma / run_sound_fifo_dma) moves all
  words instantaneously and charges **0** cycles to `g_runtime_cycles`. Real HW
  steals bus cycles during DMA (CPU stalls).

GBATEK cost model (to implement): transfer of `n` units costs read `1N+(n-1)S` +
write `1N+(n-1)S` (+2 startup I-cycles), N/S per source/dest region wait-states —
available via the existing `GbaBus::access_cycles(region,width,seq)`.

**FIXED + VALIDATED (2026-06-28).** Implemented DMA cycle-stealing: each DMA loop
(`gba_io.cpp` run_immediate_dma / run_timed_dma / run_sound_fifo_dma) computes its
GBATEK transfer cost (`dma_transfer_cost` = read 1N+(n-1)S + write 1N+(n-1)S + 2
startup, N/S from `GbaBus::access_cycles`) and accumulates it in
`dma_steal_cycles_`. The runtime drains it (`drain_dma_steal` in
`runtime_bus_bridge.cpp`) OUTSIDE `tick_devices` — at the end of `runtime_tick`
(timed/FIFO DMA) and in `runtime_resync_horizon` (immediate DMA after a guest
write) — charging it to `g_runtime_cycles` + advancing devices for the steal
window + re-arming the horizon (a `DeviceTickGuard` prevents re-entrancy). This
mirrors the existing wake-from-HALT cycle-charge pattern.

Validation (MinishCap, recomp vs NBA):
- **Steady-state per-frame cycles = 280896 EXACT** (frame-locked; the DMA cost is
  absorbed intra-frame as the idle-loop spin fills less — frame length unchanged).
- **Intra-frame cycle accuracy IMPROVED: boot-anchor skew +42 → +2 cyc** (DMA
  cycles now land where the cycle-accurate oracle puts them).
- Determinism cold×2 identical; **state_hash @frame120 byte-identical to pre-fix**
  (VRAM/PAL/OAM/cycles unchanged at frame boundaries); video COMPOSITOR-FAITHFUL;
  audio chime r=0.9903 (was 0.9906). Zero regression.

Notes: (a) the FIFO intra-burst mmio timestamps stay coincident (cost charged as a
post-burst lump) — cycle-accurate at burst level; per-word timestamp spread is a
measurement cosmetic, not an accuracy gap. (b) Cross-game (FireRed/RSE) regression
of this core-timing change is a follow-up before merge to main (the cost model is
general/GBATEK-faithful; only MinishCap is validated here).

## Cross-game validation — FireRed (game #2, 2026-06-28)

Built `FireRedLeafGreenRecomp/build-accuracy` (FireRedRecomp.exe) against the
instrumented worktree (`-DGBARECOMP_ROOT`); run with CWD=`variants/firered`,
`game.toml`. Full sweep vs NBA(firered):

- **DMA fix generalizes** (the gate to merge it): determinism cold×2 identical
  (`a7093cb0…`); per-frame cycles **280896 exact**, matches NBA. The core-timing
  change holds on a second game.
- **Video: COMPOSITOR-FAITHFUL** (frame 6 input-aligned, pixel-identical) — the
  phase-aligned comparator works cross-game.
- **IRQ: ZERO-DRIFT** — FireRed *also* arms external-clock SIO for link detection,
  so it reproduces the NBA fake-SIO Serial artifact (2362 takes), correctly
  annotated+excluded; VBlank 144/144 +0.
- **Audio chime: r=0.9903** (BIOS chime, identical).
- **MMIO:** first-divergence = the same benign `0x04000410` BIOS write; remaining
  count deltas (FIFO −16 boundary noise; IME −7; DISPCNT/affine/HALTCNT −1) all
  trace to timeline phase + NBA's extra fake-SIO handler activity — **no new
  FireRed-specific recomp bug**.

Significance: FireRed **reproduces the axis-3 (fake-SIO) and axis-5 (capture-phase)
artifacts**, confirming they are oracle/measurement properties, not MinishCap
quirks — and the recomp is faithful on both games across every axis swept. Gen3
RTC determinism (RSE/FRLG) was NOT exercised in the first 120 frames (cold×2
identical); a later-frame RTC probe is the remaining per-game item.

## Serial burndown queue (drift-driven; one behavioral change at a time)

Each item: re-measure to confirm, apply the SINGLE behavioral fix, re-measure to
GREEN, then next. Re-prioritized by the measured sweep (confidence it's a recomp
bug × blast radius). Per the LESSON: a divergence is a HYPOTHESIS — cross-check
GBATEK + confirm which side is right BEFORE coding.

1. ~~**PPU compositor divergence** (5)~~ — **RESOLVED 2026-06-28** (see Axis-5
   resolution): compositor is faithful (pixel-exact at matched-register frames);
   the drift was a video capture-phase artifact during a per-frame BLDALPHA fade
   ramp. No recomp change; fix is to phase-align `diff_video_nba.py`.
2. ~~**IRQ serial-source divergence** (3)~~ — **RESOLVED 2026-06-28** (see
   Axis-3 resolution above): NBA fake-completes external-clock SIO; recomp is
   faithful. Added internal-clock SIO model for completeness. No further recomp
   action for MinishCap; the oracle is the inaccurate side here.
3. ~~**DMA cycle-stealing** (4)~~ — **FIXED + VALIDATED 2026-06-28** (see Axis-4
   investigation): DMA now charges its GBATEK transfer cost; intra-frame cycle
   skew +42→+2, zero regression. First genuinely-real recomp gap, closed.

   **`0x04000410` residual — ASSESSED, defer.** recomp's IO map is `kIoSize=0x400`,
   so the BIOS's undocumented `0xFF` write to 0x410 falls outside it and is dropped
   at the range gate (before the mmio tap) — NBA stores+taps it. Nothing in the
   codebase reads 0x410 (functionally inert). The faithful fix (widen the IO map)
   would change the version-locked savestate layout for zero functional gain →
   deferred (constrain-surface-area). The mmio first-divergence at 0x410 is this
   benign tap-coverage difference, not a behavioral bug.
4. ~~**Audio loudness calibration** (5a)~~ — **ASSESSED, not a blind fix.** The
   0.021 "ratio" compares DIFFERENT output domains: recomp emits a final int16
   mix (peak ~19200, ~0.59 of int16 full-scale — good headroom) while the NBA tap
   is the **pre-`/0x200` raw mixer** sample (peak ~400, ~0.78 of its ±512 domain).
   Both use a sane fraction of their own full-scale; the magnitudes aren't
   comparable. The drift-tolerant chime r=0.99 already proves faithful *shape*,
   and absolute loudness is user-confirmed. The meaningful remaining check is the
   **PSG-vs-DirectSound balance**, which needs a PSG-active scene → folded into
   the PSG per-channel bit-check (deferred), NOT a blind `kGain` change.
5. **WAITCNT dynamic + GamePak prefetch timing** (2) — the +42-cyc boot skew;
   small here, will grow on WAITCNT-heavy code. Cycle-ruler more anchors first.
6. **DMA cycle-stealing** (4) — DMA charges 0 cycles; measure with a DMA-heavy
   anchor (related to the FIFO finding).
7. **HW test-ROM harness** (1) — jsmolka/mGBA-suite native-vs-NBA to close gate-(b).
8. ~~**RTC fixed epoch** (7)~~ — **ALREADY IMPLEMENTED.** `GbaRtc` honors
   `RECOMP_RTC_EPOCH=<unix-ts>` (gba_rtc.cpp:94 → `have_fixed_`/`base_now()`),
   making RTC deterministic for oracle/replay; default = host time for real play
   (+ `RECOMP_RTC_OFF`). Only RSE (Ruby/Sapphire/Emerald) have RTC hardware —
   FRLG/MC have none (their cold×2 determinism is already identical without the
   knob). Remaining: have the oracle/determinism tooling SET the epoch when
   bringing up RSE, and validate a real RTC-read scenario is deterministic with
   it. Not a missing fix — a knob to use.
9. **Mono→stereo** (5a) — after mono audio is GREEN.

## Phasing (burndown order)

1. **Audio slice** (this session): capture ring + drift comparator + first
   recomp-vs-oracle numbers. Establishes the methodology end-to-end.
2. **NBA oracle online**: cycle + pre-resample audio. Flips the gate-(b) lever
   for axes 2/3/5a.
3. **Cycle ruler** (Axis 2): WAITCNT + prefetch model, Δcyc vs NBA.
4. **DMA cycles** (Axis 4) + **absolute-timestamp event queue** (Axis 3).
5. **Test-ROM harness** (Axis 1/6): jsmolka + mGBA suite native-vs-NBA.
6. **PPU sub-scanline + bitmap modes** (Axis 5); **RTC epoch + PSG serialize**
   (Axis 7).
7. Per-game burndown: each game must read `FULLY_STATIC` (Axis 6) and pass its
   axis-5a audio slice before it is called GREEN.

> Per game CLAUDE.md: validate everything against the oracle on the same input
> BEFORE applying any fix; no stubs, no skip directives, no edits to generated C,
> fix the recompiler/runtime/TOML. A research-claimed divergence is a hypothesis.
