# COSIM_ORACLE.md — GBA first-divergence decision procedure

Status: **DESIGN — plan-first, no engine code yet** (started 2026-07-01, worktree
`gbarecomp-wt-cosim`, branch `cosim/gba-first-divergence`, based on `main` cc0284a).
This is the durable spec so the plan survives handoffs. It is the GBA instantiation of
`recomp-template/DIFFERENTIAL-COSIMULATION.md` (the agnostic decision procedure) and
`recomp-template/GBA/DIFFERENTIAL-COSIM-PROPOSAL.md` (the GBA proposal). Read those two
first; this grounds them in the actual gbarecomp tree and mirrors the PSX gold standard
(`psxrecomp/_wt-tomba2/psxrecomp/{COSIM_ORACLE.md,runtime/src/cosim.c,runtime/src/cosim_state.c,runtime/include/cosim_state.h,tools/cosim.py}`).

## Why this exists

The GBA accuracy burndown (`GBA_ACCURACY_BURNDOWN.md`, 7-axis MinishCap+FireRed sweep
vs NanoBoyAdvance) records strong verdicts: steady-state per-frame cycles bit-identical
(280896=280896); axis-3 Serial-IRQ + axis-5 pixel drifts charged to *oracle/measurement
artifacts*; axis-4 DMA cycle-stealing FIXED (+42→+2); only WAITCNT/prefetch (a +42-cycle
boot skew) + audio loudness remain. **Every one of those is a HYPOTHESIS this tool exists
to TEST, not a fact to build around.** The existing GBA comparators are
`DIVERGENCE-INSTRUMENTS`-shaped — two *free-running* processes (`mmio_cap`, `irq_cap`,
`cyc_anchor`, `audio_cap`, `state_hash`) aligned *after the fact* by a hardware anchor.
That is exactly why two of three "drifts" were charged to the oracle: after-the-fact
alignment leaves ambiguity about the *first* cause. This tool is the escalation to
**deterministic full-state lockstep** that halts at the FIRST divergence by construction —
no hypothesis can survive it that shouldn't. It neither assumes nor privileges the
burndown's verdicts; the per-subsystem sub-hash TELLS us which subsystem split first.

## The two implementations to compare

**This is done EXACTLY like psxrecomp** (user-directed 2026-07-01): the primary pairing
is the recomp backend vs the **in-project interpreter**, same binary, selected by a
force-interp flag — the direct analogue of PSX's compiled vs `PSX_FORCE_INTERP`. The
agnostic doc calls "two you own in the same project (recompiled backend vs in-project
interpreter/reference core)" the IDEAL, precisely because both are deterministic, ours,
and single-threaded in one process family — the cleanest possible Gate-1/2. **Oracle
independence is NOT a requirement for this procedure** (that rule governs the
after-the-fact ring-based device-layer diffing, not full-state lockstep — see
`feedback_differential_oracle`'s cosim exception). The value here is finding the first
full-state divergence between the two backends WE SHIP.

1. **recomp runtime vs the whole-program interpreter backend** — THE pairing.
   ⚠️ **This mode does not exist yet.** gbarecomp's `src/armv4t/interpreter.cpp` is used
   only as a *self-heal fallback* at dispatch misses and as the `bios_smoke` savestate
   oracle — there is no "interpret everything from reset" backend like PSX's
   `PSX_FORCE_INTERP`. Building it is prerequisite #1 (Build plan §1).
2. **recomp-vs-recomp** — used for **Gate 1 (determinism) only**; needs NO second backend.
   `state_hash` cold×2 already reports byte-identical on MinishCap, so this gate is nearly
   in hand. Gates 3/4 (inject, hash-vs-byte) also stand up on this pair alone. ⇒ The whole
   gate harness is provable before the interp backend is even wired.
3. **recomp vs NanoBoyAdvance** — OPTIONAL later accuracy escalation, NOT a prerequisite
   (`F:\Projects\gbarecomp\_nba_oracle\nba_oracle.cpp` + NanoBoyAdvance submodule; GPL-3.0,
   isolated binary, TCP :19844; models Game-Pak prefetch/waitstates/open-bus + absolute
   master-cycle scheduler). If pursued: NBA free-runs today; add `stride`/`step`/`park` +
   `full_state` so it parks at the launch-fixed cycle stride.

## The shared alignment clock

The **ARM7 master cycle counter at 16.78 MHz** = `g_runtime_cycles`. Verified advance
sites (`src/runtime/runtime_bus_bridge.cpp`): `:608 g_runtime_cycles += cycles;` (the
per-instruction charge — the primary checkpoint hook site), `:557` (DMA cycle-steal),
`:653` (`kIrqWakeDelayCycles` HALT-wake), `:742` (idle-loop skip). NBA exposes
`scheduler.GetTimestampNow()`; mGBA exposes `mTimingCurrentTime`. **Checkpoint on strides
of this master cycle** so both sides park at the SAME cycle. Fix the stride at LAUNCH
(env `GBA_COSIM_STRIDE`) before either process runs an instruction — no set-stride race.
Compare on the clock-keyed full-state chain hash, NOT on block leaders (the recomp's CFG
blocks and the interpreter's per-instruction steps do not align); keep a `last_leader_pc`
stash for human-readable reporting only.

GBA timing surfaces (each a divided function of this one clock, each a prime divergence
surface kept in the hash): PPU cadence (4 cyc/dot, 308 dots/line, 228 lines — VCount /
HBlank edges); timer prescaler phase (1/64/256/1024) + cascade; DMA-sound FIFO refill
(timer 0/1 overflow → FIFO drain → DMA1/2 → IRQ, `run_sound_fifo_dma`, `gba_io.cpp`).

## Full architectural state — the completeness checklist

Hash EVERY quantity that can influence future guest execution, and NOTHING host-only. A
missed execution-relevant field = a blind spot (false "no divergence"); an included
host-only field = a false positive. **Do NOT trim on any burndown axis being GREEN** —
that assumes the very thing under test. The existing `state_hash`
(`src/debug/tcp_debug_server.cpp:566`) hashes ONLY IWRAM+EWRAM+VRAM+PAL+OAM+`g_runtime_cycles`
— it OMITS the entire CPU register file, CPSR/SPSR/banked regs, and every device's
timing/scheduling state. A co-sim on that hash is blind to a timer-phase or IRQ-take-point
divergence. The new `cosim_state_hash` must cover ALL of:

- **ARM7TDMI (`ArmCpuState`, `src/armv4t/runtime_arm_types.h`)**: `R[0..15]` (see PC
  caveat), packed `cpsr`, `banked_sp[6]`, `banked_lr[6]`, `banked_spsr[6]`,
  `r8_12_user[5]`, `r8_12_fiq[5]`. Mode/THUMB bit = `cpsr` bit 5 (`CPSR_T_BIT`).
- **Pipeline / prefetch micro-state**: the +8/+4 fetch-offset currency and the Game-Pak
  **prefetch buffer** (`prefetch_word`, `gba_bus.cpp`). Save-states OMIT this class — it
  is the classic blind spot and is exactly the axis-2 WAITCNT/prefetch gap.
- **Memory**: EWRAM (256 KB @ 0x02000000), IWRAM (32 KB @ 0x03000000), VRAM (96 KB),
  OAM (1 KB), PAL (1 KB). RAM via **incremental page-hash on write** (mirror PSX
  `cosim_note_ram_write`: mark 4 KB pages dirty on the write chokepoint, recompute lazily
  in `cosim_state_hash`); the small blobs struct-hash directly.
- **PPU full state** (`gba_ppu.cpp`): all LCD I/O (DISPCNT..BLDY), window state
  (WIN0/1/IN/OUT), the **internal affine BG reference-point latches** (BG2X/Y, BG3X/Y
  accumulators — distinct from the written regs, updated per scanline), current
  scanline/dot (VCOUNT + dot phase).
- **Audio full state** (`gba_audio.h`): both Direct-Sound FIFOs + which timer clocks each
  (SOUNDCNT_H) + current sample; all 4 PSG channels — SOUND1 sweep/duty/envelope/length
  (`sweep_cycles`, `envelope_cycles`, `length_cycles`, `waveform_phase`), SOUND2, SOUND3
  wave RAM + position, SOUND4 noise LFSR; SOUNDCNT_L/H/X; `samples_generated_`. **AUDIT:
  burndown axis-7 notes SOUND3/4 are NOT serialized in the save-state — do not inherit
  that gap into the co-sim hash.** EXCLUDE the host SDL audio buffer/resampler.
- **4 DMA channels** (`gba_dma.{h,cpp}`, `gba_io.cpp`): src/dst/count/control PLUS the
  **internal latched src/dst/remaining**, the DMA-sound/HBlank/video-capture (DMA3) mode
  state, and the `dma_steal_cycles_` accumulator (the cycle-stealing fix).
- **4 timers** (`gba_timers.{h,cpp}`): counter/reload/control, cascade linkage, and the
  **prescaler phase accumulator** (a phase, not a frame quantity).
- **Interrupt controller** (`gba_irq.h`): IE / IF / IME + pending/latched state, the
  HALT-wake latency (`kIrqWakeDelayCycles`), the BIOS IRQ handshake
  (LR_irq/SPSR_irq/mode/PC=0x18 entry, 0x03007FFC handler pointer).
- **WAITCNT** (`gba_bus.cpp`, `access_cycles`): changes cycle timing though not "data" —
  a divergence here IS a real cycle divergence (the axis-2 surface).
- **Routing / SMC state**: any dirty-RAM / self-heal overlay bitmap that affects
  dispatch/interp routing.

**EXCLUDE (host-only — false divergences otherwise):** SDL/host audio buffers &
resampler, function pointers, the self-heal JIT overlay tables / DLL handles,
generated-dispatch pointers, malloc addresses, file handles, struct padding, uninitialized
bytes. Serialize little-endian explicitly; arrays in fixed order; any event/deadline queue
SORTED by a deterministic key.

**DELIBERATELY EXCLUDE emulator bookkeeping that legitimately differs by backend without
being a guest divergence** — e.g. an interrupt-poll CALL count (recomp polls per-block,
interp per-instruction). Hash the GUEST-ARCHITECTURAL quantity (cycles-since-event),
never the poll-call count (mirror PSX's `interrupts_cosim_hash` refinement).

*PC caveat (`R[15]`):* the recomp keeps `R[15]` current only at dispatch boundaries
(transiently stale/zero between dispatches) while the interpreter keeps it exact
per-instruction. **Exclude PC from the cross-impl hash** (still report it via a `cpu`
dump). A real control-flow split shows up as a differing GPR/memory value within one
checkpoint, delaying detection by at most one checkpoint — never a blind spot. (Verified
on PSX: at its first flagged divergence only PC differed; every other field matched.)

## Validation gates — trust NOTHING until these pass

Run on recomp-vs-recomp FIRST (needs no second backend); repeat for interp-vs-interp once
that backend exists, and NBA×2 before believing recomp-vs-NBA.

1. **recomp-vs-recomp = 0 divergence** across the attract run. Proves determinism +
   hashing + that all host-only state is excluded. Likely nondeterminism suspects: the
   audio thread / SDL sink (force headless, single-thread, no host audio), and the **RTC
   host-time leak** (`gba_rtc.cpp` reads `std::time`) — pin it with `RECOMP_RTC_EPOCH`.
   MinishCap has no RTC (cold×2 already identical → cleanest first fixture); FRLG/RSE
   MUST set the epoch knob.
2. **interp-vs-interp = 0** (after the whole-program interp backend exists) / **NBA×2 = 0**
   (headless deterministic mode).
3. **Injected fault halts at the right cycle + names the subsystem.** A knob XORs one
   IWRAM byte / one timer reload / one IF bit in one instance after checkpoint K; the tool
   MUST halt at ~K and localize it. This is the ONLY gate that catches a silently-blind
   coordinator (a parser bug comparing `None==None` passes gate 1 while catching nothing).
   Apply the injection to LIVE state so it flows into the hash naturally (mirror PSX
   `cosim_inject_ram`/`_reg`). Add a hard assert that each compared sub-hash parsed
   non-null.
4. **Hash-vs-byte audit** every N checkpoints: force a full byte compare even when hashes
   match — proves incremental page-hash-on-write maintenance is correct.

Only after the relevant 1–4 pass do we run the A-vs-B and believe its first-divergence
report.

## Determinism via the no-input attract fixture

Use a **no-input attract/demo** so both sides receive identical (empty) input by
construction (agnostic-doc requirement 2; user note: "don't over-worry determinism, but
Gate 1 must pass"):
- **BIOS chime + Nintendo logo** (~2 s, no input) — smallest fixture; runs for BIOS-only
  `tools/bios_smoke`.
- **MinishCap** title → intro cutscene (flagship / burndown pilot): boots fully native,
  0 interpreted, 0 dispatch misses; burndown ran 800 frames no input.
- **FireRed** intro (cross-game gate; already swept vs NBA).
- Gen3 titles each have an attract loop for later per-game passes.

## Falsifiable predictions (the tool decides, not the doc — do NOT code toward these)

- **If the burndown is right:** past the boot transient the chain-hash matches for a long
  attract window; the first divergence (if any) sits in early boot and its sub-hash points
  at the **prefetch/WAITCNT** surface (the +42-cycle skew), with CPU+RAM+PPU+audio matching.
- **If it is wrong,** the tool shows it: the DMA-sound FIFO/timer sub-hash splits first
  (cycle-stealing fix imperfect off-MinishCap), or an IRQ take-point lands one instruction
  off (axis-3 "edge-quantized not cycle-exact"), or a PPU affine latch diverges on a
  mid-frame-scroll game, or Gate 1 fails (RTC/audio-thread nondeterminism masquerading as
  drift). Each is a different fix. Build the neutral full-state co-sim, pass the gates,
  read which sub-hash splits first, THEN iterate.

## Build plan (mirror the PSX `psx-cosim`; all engine code `#ifdef GBA_COSIM`)

- [x] `COSIM_ORACLE.md` (this file).
- [ ] **§1 Whole-program interpreter backend** (`GBARECOMP_FORCE_INTERP`, or a build-time
      `GBA_COSIM` sub-mode). Make `runtime_dispatch` route EVERY target PC through the
      interpreter instead of calling the generated function. MUST NOT touch generated C.
      **DE-RISKED 2026-07-01 — the driver already exists; this is a routing change, not a
      new interpreter:**
      - The interpreter (`src/armv4t/interpreter.cpp`, 1134 L) is effectively complete for
        real code: S-bit LDM/STM incl. the privileged/exception-return path (761–831) and
        MSR bank-switch (1085–1089) are implemented; the only `NotImplemented` (1101) is
        the catch-all `default` (a genuine gap would abort loudly — the honest behavior).
      - A complete whole-program fetch/decode/`step`/SWI/IRQ loop ALREADY exists twice:
        the self-heal bridge `src/runtime/runtime_arm_default_aborts.cpp:503+` (syncs
        interp↔`g_cpu` per instruction, decodes ARM/THUMB, routes SWI through the
        recompiled BIOS, aborts on interpreter gaps) and `tools/bios_smoke/main.cpp:606`
        (the same loop driven from reset).
      - §1 = under the force-interp flag, have `runtime_dispatch(pc)` (called from
        `runtime.cpp` `step_once` → `runtime_dispatch(g_cpu.R[15])`) enter that bridge loop
        unconditionally (treat every PC as a miss) and run it as the top-level driver, with
        recompilation OFF (`GBARECOMP_SELFHEAL_RECOMPILE=0` is the seed). Model the
        top-level driver on the bios_smoke loop + the bridge's SWI/IRQ handling.
- [ ] **§2 State-hash module** `src/debug/cosim_state.{h,cpp}` (mirror PSX
      `cosim_state.{h,c}`): canonical FNV-1a-64 serialize → incremental IWRAM/EWRAM page
      hashes + full `ArmCpuState` (PC excluded) + PPU/audio/DMA/timer/IRQ/WAITCNT device
      sub-hashes + gate-4 live-state injection. Reuse the version-locked serializers in
      `src/debug/snapshot.{h,cpp}` (SnapshotWriter/Reader) for the bulk device blobs and
      **AUDIT each for the omitted timing fields** (per its docstring the snapshot is taken
      at a clean dispatch boundary — confirm it captures mid-DMA / prescaler phase / FIFO,
      not just register values; add what it omits).
- [ ] **§3 Engine** `src/debug/cosim.cpp` (mirror PSX `cosim.c`): cycle-keyed checkpoint
      (ring + cumulative chain hash + park/step lockstep) + minimal standalone TCP server.
      Commands: `status`/`chain`/`stride N`/`step N`/`hash`/`sub`/`window N`/`cpu`/`dev`/
      `inject ram|reg`/`reset`. Hook `cosim_tick()` into the `g_runtime_cycles += cycles`
      advance (`runtime_bus_bridge.cpp:608`); `cosim_note_ram_write` in the IWRAM/EWRAM
      write chokepoints (covers CPU + DMA writes); `cosim_init()` at startup.
- [ ] **§4 Clean `gba-cosim` build target** (CMake): `GBA_COSIM` + diagnostics OFF,
      single-threaded guest, headless (no host audio/video sink), no host throttle. Add
      `cosim.cpp` + `cosim_state.cpp` to the target. Determinism knobs forced:
      `RECOMP_RTC_EPOCH`, headless, single-thread. Build via **PowerShell** (Bash sandbox
      forces TEMP=C:\Windows → c++ fails — see `project_widescreen_stepB_done`).
- [ ] **§5 Coordinator** `oracle/gba_cosim.py` (sibling to `nba_common.py` /
      `diff_*_nba.py`): launch recomp (:19842-class) + peer on the same attract with no
      input, master-cycle lockstep via `step`, compare `chain`; on mismatch print `sub`
      diff + `cpu`/`dev` field dumps + `window` from both. Reuse `nba_common.Client`.
      MUST checkpoint-lockstep, NOT free-run.
- [ ] **§6 Gates 1/3/4 on recomp-vs-recomp** (determinism, injected-fault, hash-vs-byte).
- [ ] **§7 Interp backend up → gate 2 (interp×2) → recomp-vs-interp** A-vs-B — THE run.
- [ ] **§8 (optional accuracy escalation) NBA reference peer**: add `stride`/`step`/`park`
      + `full_state` to `_nba_oracle/nba_oracle.cpp` (scheduler has absolute timestamps) →
      NBA×2 gate → recomp-vs-NBA. Not a prerequisite; pursue only if the interp pair's
      accuracy verdict needs an external cross-check.
- [ ] **§9 Iterate**: bracket → field-diff → NAME → faithful fix (in recompiler/runtime,
      never generated C, never a stub/skip) → rebuild → re-run. Final acceptance is
      user-visible pixels/audio, never "hashes match" alone.

## Pitfalls (each cost real time on PSX)

- **Silently-blind coordinator** — a parse bug making every compare equal. Gate 3 is the
  only catch. Hard-assert compared fields parsed non-null. (PSX hit exactly this: a
  stride-2 parser misaligned on the leading `parked` word → chain=None both sides →
  None==None "equal" forever.)
- **Racy park** (free-run + async stop): two processes stop at different cycles. Use
  checkpoint-lockstep with launch-fixed stride.
- **PC / currency mismatch:** exclude PC from the hash; field-diff any early divergence
  before believing it (may be representational, not guest).
- **Incomplete device snapshot:** if a serializer omits its scheduled next-event cycle /
  FIFO / prescaler phase, a real timing divergence there is invisible. Audit each.
- **Side-effect-full readback:** snapshot readers must not mutate guest state (some
  register reads toggle "ready" bits). Use pure-read accessors, or read only at a halt.

## Pointers (verified in-tree, 2026-07-01, worktree gbarecomp-wt-cosim)

- Method + exemplars: `recomp-template/DIFFERENTIAL-COSIMULATION.md`,
  `recomp-template/GBA/DIFFERENTIAL-COSIM-PROPOSAL.md`,
  `recomp-template/SNES/DIFFERENTIAL-COSIM-PROPOSAL.md`.
- **PSX gold standard to mirror:** `psxrecomp/_wt-tomba2/psxrecomp/COSIM_ORACLE.md` +
  `runtime/{include/cosim_state.h,src/cosim.c,src/cosim_state.c}` + `tools/cosim.py`.
- Burndown / hypotheses under test: `GBA_ACCURACY_BURNDOWN.md`, `ISSUES.md`.
- CPU state: `src/armv4t/runtime_arm_types.h` (`ArmCpuState`); interpreter:
  `src/armv4t/interpreter.cpp`; master clock `g_runtime_cycles`
  (`src/runtime/runtime_bus_bridge.cpp:608`); driver `src/runtime/runtime.cpp` `step_once`.
- Devices: `src/gba/gba_ppu.cpp`, `gba_timers.{h,cpp}`, `gba_dma.{h,cpp}` + `gba_io.cpp`
  (`run_immediate_dma`/`run_timed_dma`/`run_sound_fifo_dma`), `gba_audio.{h,cpp}`,
  `gba_irq.h`, `gba_bus.cpp` (`access_cycles`, `prefetch_word`, WAITCNT), `gba_rtc.cpp`
  (`RECOMP_RTC_EPOCH`).
- Existing hash + rings: `src/debug/tcp_debug_server.cpp` (`cmd_state_hash:566`,
  `mmio_cap`, `irq_cap`, `cyc_anchor`, `audio_cap`); serializers `src/debug/snapshot.{h,cpp}`.
- Oracles: `F:\Projects\gbarecomp\_nba_oracle\nba_oracle.cpp` (NanoBoyAdvance, :19844);
  `oracle/` mGBA bridge (:19843); comparator plumbing `oracle/nba_common.py` +
  `oracle/diff_{cycle,irq,mmio,video,audio_drift}_nba.py`.
- Fixtures (no input): BIOS chime (`tools/bios_smoke`), MinishCap title/intro, FireRed
  intro; Gen3 attract loops.

## History (do not lose)

- 2026-07-01: worktree `gbarecomp-wt-cosim` created off `main` (superset of
  `accuracy/gba-burndown`); PSX gold standard studied; NBA oracle source confirmed present
  at `_nba_oracle/`; gate harness confirmed provable on recomp-vs-recomp alone. Decision
  (user): do it EXACTLY like psxrecomp — recomp vs in-project interpreter is THE pairing;
  independence not required for the procedure; NBA optional. This doc written.
- 2026-07-01: §1 DE-RISKED by inspection (no code yet): the interpreter is effectively
  complete and a whole-program fetch/decode/step/SWI/IRQ driver already exists (self-heal
  bridge + bios_smoke). §1 reduces to routing `runtime_dispatch` through it under a
  force-interp flag. Awaiting sign-off before writing engine code (plan-first gate).
- 2026-07-01: BUILT §1–§6 (user go-ahead "build it all"). §1 force-interp is a
  per-instruction driver reusing runtime_tick/runtime_swi (NOT hooking runtime_dispatch
  — the interpreter handles guest branches itself; only SWI/IRQ route through recompiled
  BIOS). Checkpoint hook is in runtime_tick (verified per-instruction in both backends via
  arm_codegen). io hash uses GbaIo::cosim_hash() (architectural only) not serialize()
  (which carries bookkeeping counters). ALL GATES PASS (recomp²=0, interp²=0, inject
  halts+localizes). Iteration: (1) FIXED prefetch split (force-interp now calls
  runtime_should_yield() = the generated prologue's BIOS-prefetch latch); (2) FIXED io
  bookkeeping false-positive (excluded unmapped_count_/dma_runs_/dma_words_); (3) OPEN
  genuine divergence — timer0 counter off ~6 at cycle ~1.6M (frame 5, BIOS pc~0x1774),
  timer enabled prescaler/1 ⇒ TM0 enabled ~6 master-cycles apart = a per-instruction
  cycle-charge skew between the recomp codegen cost model and the interpreter's
  Interpreter::step insn_cycles. NEXT: drill to the exact instruction and reconcile the
  two cost models (the real accuracy burndown). Commits 5f847c0/268351d/ad3f8e2/095d990.
- 2026-07-02: MinishCap burndown past frame 272 — TWO first-divergences root-caused + fixed
  (see ISSUES.md LP-005/LP-006). (a) **LP-005** (commit 2b265bc): recompiler `arm_codegen.cpp`
  dropped the pipeline-refill cycles on the `movs/subs pc,lr` exception-return (the BIOS SWI-return
  idiom at 0x188) — computed `_cyc=3` but `return`ed before the tick, while its sibling LDM
  exception-return + normal PC-write paths tick correctly. Charged 0 not 3; surfaced only when
  force-interp interpreted a VBlank-yield-resumed BIOS handler. Fixed → fp cycle-trace 0/7,362,465.
  (b) **LP-006** (commit b73f574): the reference interpreter didn't bank R8..R12 on mode switch (its
  own acknowledged gap) while the runtime's bank_out/bank_in do — left `r8_12_user` stale, a cpu-subhash
  split with every mode-visible reg matching. Added `swap_r8_12_banks()` at the interpreter's MSR /
  exception_return / restore_cpsr_from_spsr sites (enter_irq/enter_swi bios_smoke-only, deferred).
  Diagnostics that cracked both (commit 9ed5db4): env-gated `cyc_probe` + `g_tick_ctx` tick-origin
  tags + cosim `cpu` dump ur/fr banks. MinishCap now CLEAN through cp1280/frame ~298 (max 90M cyc),
  all checkpoints identical chains. L1 codegen harness 128/128. Both fixes BIOS-level (game-independent).
  BREADTH (all carts regenerated + rebuilt with the fixes, recomp-vs-interp, stride 65536, max 90M cyc):
  **MinishCap, LeafGreen, AND Emerald all cosim CLEAN through cp1280 / frame ~298** — every checkpoint
  identical chains, "no divergence within --max". The BIOS-level fixes hold across all three games; no
  game-specific divergence surfaced through frame ~298. Emerald build-cosim newly configured this session
  (mirrors FRLG: GBA_COSIM=ON, GBARECOMP_ROOT=worktree). NEXT unexplored: run past frame ~320 (attract
  loops), or into input/gameplay, to surface deeper divergences; NBA §8 independent-oracle still unbuilt.
