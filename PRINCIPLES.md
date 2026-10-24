# gbarecomp Project Principles

These rules are platform-specific specializations of the cross-project
recomp principles in [recomp-template](../../recomp-template/PRINCIPLES.md).
Platform-agnostic rules still apply; this file adds GBA detail.

## Ground Truth

- The GBA ROM (verified by SHA-1 against a known-good hash) and a
  trusted hardware oracle are the behavioral source of truth.
  Primary oracle: **mGBA**. Secondary cross-check:
  **NanoBoyAdvance**. Tertiary (for automation): **SkyEmu**.
- Generated C/C++ is evidence, not authority. If generated code is
  wrong, fix the recompiler, runtime, analyzer, or game metadata —
  then regenerate.
- Disassembly / decomp resources (e.g., `zeldaret/tmc`) are valid
  sources for symbols, function boundaries, ROM layout, asset labels,
  and linker knowledge. They are **not** an execution oracle. The
  emulator is.

## Maintain the real BIOS reference

The LLE build executes the verified retail BIOS through the generated dispatch
table. SWIs enter at `0x00000008`; IRQs enter at `0x00000018` and execute the
original dispatcher, handler and exception return. Maintain the BIOS generation
configuration, including its reviewed interior entries. Report interpreted or
healed execution honestly and fix discovery/codegen at its source.

The user's BIOS dump is hash-verified; see `bios/README.md`. Its recompiled
execution is the in-tree LLE reference. mGBA and NanoBoyAdvance provide independent
evidence for device or timing bugs shared by our CPU implementations.

The BIOS intro baseline was completed in Phase 2.7. Keep relevant BIOS tests
working when changing that path. A known BIOS defect must be investigated in
its affected scope; unrelated cartridge or HLE work does not require proving
every part of the machine flawless again. Record limitations rather than
declaring an incomplete reference perfect.

### Boot presentation and BIOS services are separate choices

`[bios].skip_intro` selects a documented post-boot handoff instead of replaying
the logo/chime. It does not select the SWI implementation. Skipping boot can
change the initial IRQ phase and reachable resume entries, so qualify coverage
for the actual title configuration.

BIOS services may have native HLE implementations under [the HLE policy](#hle-policy).
Preserve their caller-required results, memory semantics, continuation and
completion ordering. Do not return fabricated success or use HLE to hide a
missing LLE implementation. Exact instruction counts and every internal BIOS
stall are requirements of the faithful route, not universal HLE requirements.

## Honest self-healing — interpreter may bridge, must heal to static + report (SHOWSTOPPER)

*Supersedes the former "Interpreter is informative, never
load-bearing" absolute rule (relaxed by the project owner 2026-06-13).
History: that rule banned the interpreter from every runtime path. It
was protecting **honesty**, not purity — and the absolute form
couldn't admit genuinely-dynamic RAM code the offline finder can't
see. The honesty requirement is kept and made stricter; the purity
requirement is dropped.*

The `armv4t::Interpreter` MAY now bridge a dispatch miss on the runtime
hot path — for BIOS, ROM, or RAM code — but ONLY under all three of
these conditions, **together**:

1. **Loudly documented — never silent.** Every fallback is observable
   and honestly reported: a session notice appears on the first miss,
   every PC is counted and dumped in a coverage report at exit, and
   detail is queryable live. The proposal frag and coverage JSON are
   also rewritten on every new distinct miss and flushed from the
   SIGABRT handler, so an aborted or killed run keeps its harvest
   (the JSON's `phase` says `running`, `abort` or `exit`). Per-PC console lines are available with
   `GBARECOMP_SELFHEAL_VERBOSE=1`, not emitted by default, because
   synchronous console I/O must not stall gameplay. We must never be
   able to call a half-interpreted run "statically recompiled / done"
   without that fact being visible.
2. **Self-healing to static.** On the first miss we interpret to keep
   running, but we immediately recompile that function on the fly (the
   on-disk "code cache") so it is served NATIVELY for the rest of the
   run. The interpreter is a temporary bridge, never the permanent
   engine for a given PC.
3. **Fed back to the source.** Every miss appends to a master
   miss-list that seeds the per-game TOML — via a reviewed proposal
   file, NEVER an auto-write — for the NEXT full build, so coverage
   improves monotonically across runs and, aggregated from users,
   across the ecosystem.

**STILL FORBIDDEN ❌** — these mean the recompiler is broken or the
ecosystem is being misrepresented:
- A dispatch miss that *silently* interprets forever — no log, no
  self-heal, no miss record.
- Calling a build "fully static / done" while PCs were interpreted or
  served from the code cache this session, without reporting it (see
  "Coverage honesty is load-bearing").
- Treating self-healing as an excuse NOT to fix discovery/codegen: a
  ROM miss is still a finder/codegen bug to fix at the tool level (see
  "Hints Are Not Correctness"). Self-healing keeps the user playing
  while we fix it; it does not replace the fix.
- A replacement that fabricates success, hides missing behavior, or lacks
  the maintained LLE counterpart required by [HLE.md](HLE.md).

**INFORMED ✅** — the interpreter is also still our offline oracle:
decode-trace diff harnesses, per-IrOp semantic reference,
`armv4t_tests`, and stand-alone verification tools (`bios_smoke`).

**Genuine interpreter gaps still abort loudly.** If the interpreter
itself cannot model an opcode (`NotImplemented` / `Undefined`) it
aborts with a clear PC + opcode — a real, unrecoverable gate we keep.
Only the *dispatch* gate (miss → abort) is replaced by self-healing.

**How to apply:**
- `runtime_dispatch_miss(pc)` does NOT `std::abort()`. It logs the
  miss, interprets the block to bridge, triggers an on-the-fly
  recompile into the code cache, and returns; subsequent hits run
  native.
- Acceptance and coverage-regression runs set
  `GBARECOMP_STRICT_STATIC=1`. This disables overlay cache loading,
  background healing, and interpreter bridging; any dispatch miss
  aborts immediately. A successful strict run proves the linked static
  corpus covered every executed PC rather than merely reporting that a
  fallback happened to be unused.
- The bridged path must diff **bit-identical** to the mGBA oracle —
  bridging is behavior-preserving or it is a bug.
- New diff harnesses are still encouraged; the interpreter-vs-recomp
  diff is good engineering.
- This invariant applies to **every** console in this ecosystem (NES,
  SNES, PSX, Genesis, Xbox-HLE, future).

## Coverage honesty is load-bearing (SHOWSTOPPER)

A build may NOT be described as "statically recompiled," "fully
native," or "done" while any guest PC was interpreted or served from
the on-the-fly code cache this session — unless that is reported.

- The runtime maintains coverage counters (native-AOT vs
  healed-to-cache vs interpreted-only, by **distinct PC**) and dumps a
  report at exit. If anything was not native-AOT, the report says so
  loudly: `COVERAGE: NOT FULLY STATIC — N interpreted, M healed`.
- "It runs" is not the bar. "It runs, and here is exactly how much ran
  as statically-recompiled native code" is the bar.
- The master miss-list file is written for **every** user (no debug
  server required) so coverage gaps are always surfaced and shareable
  (e.g. attach to a GitHub issue).

This is the honesty the old absolute interpreter ban was protecting —
now made explicit and measurable instead of enforced by prohibition.

<a id="hle-policy"></a>
## HLE and LLE: caller contracts and build selection

Follow [HLE.md](HLE.md) and the shared [HLE policy](../../recomp-template/HLE.md)
verbatim. The developer selects an implementation when building. A qualified
HLE configuration may be the preferred release build; retain a functioning,
documented LLE build in maintained source, even when it is slower.

HLE can replace a whole algorithm or subsystem. Native math, GPU work, batching,
caches, private state and approximate internal timing are allowed. Preserve
the inputs, outputs, memory effects, callbacks, continuation and meaningful
ordering that supported callers observe. Small practically unnoticeable
differences are authorized for substantial measured gains. Material gameplay
changes, missing effects and corrupted state are defects or a separate feature
scope.

Do not require original execution underneath HLE, continuous differential
comparison, live handoff, automatic per-call LLE fallback, cross-build savestate
compatibility or a player-facing HLE/LLE toggle. Compare separate builds or
isolated fixtures against their promised contracts. Exact host optimizations
still preserve their exact promises.

The existing audio shadow and legacy BIOS runtime selectors are historical
interfaces, not requirements for new HLE. Their existence does not establish
that those replacements are qualified for every title or netplay profile.

Netplay simulation must remain bit-identical across qualified peers. Settle
the common simulation profile before frame 0 and retain it for the match.
Local presentation may differ only downstream of guest state, readbacks,
digests and rollback. See [the netplay policy](../../recomp-template/NETPLAY.md).

## Tool Skepticism

- Treat every tool result as untrusted until validated against another
  source or a known-good case.
- Validate first outputs manually: disassembler output, decoder
  results, oracle TCP responses, frame logs, screenshots, diff tools.
- If observability is missing, extend the structured debug surface
  (TCP / rings / traces / snapshots). Never build conclusions on ad
  hoc `printf` spam.

## Hints Are Not Correctness

- Symbol imports and CFG hints are bootstrap aids, not a first-class
  model of the program.
- A missing function, table, jump-table, or data shape that requires a
  hint exposes a discovery gap. The proper fix improves discovery,
  decoding, analysis, or generation — so the next game benefits too.
  The self-healing miss-list feeds a *reviewed proposal* file, never
  an auto-write to `game.toml`; a human merges it. Proposals are
  bootstrap aids toward the discovery fix, not a substitute for it.
- Use per-game config only for facts that are genuinely per-game:
  entry point, ROM identity, save-chip type, IO quirks the game
  exploits, and verified metadata.
- Do not paper over a recompiler or runtime bug with a per-game hint
  unless both a class fix and a mechanical audit are blocked, and
  document that debt.

## Control Flow Semantics (ARM/THUMB)

The ARM7TDMI is a single CPU that switches between ARM (32-bit) and
THUMB (16-bit) instruction sets at runtime via `BX`, `BLX`, mode
changes, and exception entry. Treat it as one CPU, not two.

- **Interworking is first class.** `BX` / `BLX` / `LDR PC` / `LDM
  {pc}` switch state based on bit 0 of the target. The recompiler must
  preserve this. Block discovery must follow both states.
- **PC-visible pipeline.** ARM reads of PC return `current + 8`;
  THUMB reads of PC return `current + 4`. ALU operands, address
  generation, and PC-relative loads depend on this. Model it
  explicitly.
- **Condition codes & shifter carry-out.** Every ARM data-processing
  instruction has a condition field and may produce a shifter
  carry-out that feeds CPSR.C. Both must be modeled.
- **Banked registers & modes.** SVC, IRQ, FIQ, ABT, UND each have
  banked R13/R14 (and FIQ banks more). Exception entry stores CPSR
  into SPSR_mode and switches mode. Exception return (`SUBS PC, LR,
  #imm`, `LDM ^`, `MOVS PC, LR`) restores CPSR from SPSR.
- **LDM/STM edge cases.** Empty register list, base in register list,
  `^` bit (user-mode bank or SPSR-restore), and write-back interact
  in non-trivial ways. Don't simplify.
- **SWI entry & BIOS handoff.** SWI traps to BIOS at 0x00000008.
  The LLE build handles it through recompiled BIOS dispatch; a native
  service selected by an HLE build preserves its documented caller contract.
  An uncovered LLE handler is a coverage issue, not permission to stub it.
- **IRQ entry / return.** The GBA IRQ vector model (BIOS-mediated)
  must match hardware ordering of `IE`/`IF`/`IME` and the BIOS IRQ
  return sequence. Save / restore CPSR via SPSR_irq.
- **Stack-affecting idioms** (`PUSH {LR}` / `POP {PC}`, tail calls,
  computed dispatch, BL into mid-function) are class-level
  recompiler problems. Fix the class and audit all instances.

## Runtime Boundaries

- Bus and memory primitives must be faithful and boring.
  - Region map: BIOS, EWRAM, IWRAM, IO, PAL, VRAM, OAM, ROM,
    SRAM/Flash/EEPROM, plus open-bus behavior.
  - Waitstates and prefetch buffer effects are real and visible to
    timing-sensitive code. Approximate first; refine, do not hide.
  - Mirroring rules (e.g., IWRAM mirror at 0x0300_7FFF region,
    palette/OAM mirrors) match hardware, not "nearest convenient."
- Do not mask out unmapped or unknown reads/writes silently. Every
  unmapped or unknown IO access must produce a structured trace
  artifact. Magic return values are forbidden unless documented.
- Save-state must restore guest CPU + memory + devices + a
  well-defined generated-code resume boundary. Never depend on
  resuming an old host C stack after replacing guest state.

## Debug Loop (always-on ring first)

- Find the first divergence, not the final visible bug.
- Classify the failure: discovery / codegen, runtime / timing,
  memory / bus, IO, IRQ / DMA / timer scheduling, PPU device
  emulation, audio FIFO / timing, save chip, or game metadata.
- **Never** "arm a trace then run a workload" if a ring buffer
  exists. Probes query the always-on ring for the window of interest.
  If the relevant event isn't captured, *extend* the ring, not the
  probe.
- Pause/step is a control-plane primitive for a human at a debugger,
  not a means of synchronizing two observers. Use free-run + diff
  ring.

## Validation

- A fix is done only when:
  1. Root cause is explained.
  2. The class of bug is addressed or audited across all sites.
  3. Generated code is regenerated.
  4. The platform core builds; the game (if any) builds.
  5. A deterministic smoke or oracle comparison exercises the
     behavior.
- Smoke scripts and frame logs must be deterministic. The next
  session must be able to rerun them without manual input
  reconstruction.

## Anti-patterns specific to GBA / Minish Cap

- Do **not** treat any decompilation (e.g., `zeldaret/tmc`) as
  execution truth. It is a symbol and structure reference only.
- Do **not** lift the decomp's PC-port renderer, audio mixer, or
  input layer as the runner.
- Do **not** add Minish Cap special-cases to the GBA core unless
  proven hardware behavior with an oracle backing it.
- Do **not** assume THUMB-only. Real GBA games (including Minish Cap)
  use ARM for IWRAM hot paths and IRQ handlers.
- Do **not** stub SWIs to "what the game expects." Implement or
  execute the real BIOS.
- Do **not** silence unknown IO or unmapped memory. Trace it.
- Do **not** edit `MinishCapRecomp/generated/*` to fix bugs. Fix the
  recompiler.
