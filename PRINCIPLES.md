# gbarecomp Project Principles

These rules are platform-specific specializations of the cross-project
recomp principles (see `F:\Projects\recomp-template\PRINCIPLES.md`).
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

## BIOS intro must be flawless before ROM (HARD GATE)

No ROM, cartridge, game, or "Phase 5" work happens until the GBA
BIOS intro is **flawless** on three axes simultaneously:

1. **Visually** — the rendered framebuffer matches the mGBA oracle
   pixel-for-pixel at every PPU frame from boot through the
   post-intro idle state.
2. **Audibly** — the startup chime plays correctly. The native
   audio sample stream matches the oracle's sample stream over the
   intro duration.
3. **In-memory** — OAM, VRAM, PAL, IWRAM byte-identical to the
   oracle at every PPU frame checkpoint. IO is allowed open-bus
   jitter on documented write-only fields, nothing else.

**Why:** The BIOS intro is the simplest piece of code the
emulator runs. It requires no cartridge, no game-specific symbols,
no per-ROM config. If it doesn't render correctly, every higher
layer is built on a foundation we don't trust. Every cycle of
"close enough" intro compounds into invisible bugs the moment we
load a cart.

**How to apply:**
- Treat any OAM / PAL / VRAM / IWRAM drift between native and
  oracle as a P0 bug. Do not paper over with "we'll come back."
- The "intro looks close" or "it sort of works" answer is
  unacceptable. The acceptance criterion is byte equality, not
  visual approximation.
- The audio subsystem is part of the gate. `src/gba/gba_audio.cpp`
  must stand up before the gate closes.
- The `bios_intro_flawless` ctest must run green for the gate to
  stay closed. CI failure = gate re-opens.
- See `docs/ROADMAP.md` Phase 2.7 for the work breakdown.

## BIOS is sacred — recompiled and dispatched (with honest self-healing)

The GBA BIOS is **recompiled and executed via the dispatch table**,
not stubbed, not HLE'd, not "fast-forwarded." A missed BIOS PC
self-heals like any other code (see "Honest self-healing" below) —
bridged by the interpreter, recompiled on the fly, and logged loudly —
never silently HLE'd or stubbed, with 100% recompiled BIOS coverage
the goal by construction. Every game boot path on real hardware
enters the BIOS at power-on, runs the Nintendo logo intro, then
hands control to the cartridge at `0x08000000`. Our recompiled
builds do the same thing — through recompiled code, from the very
first PC.

- The BIOS image is the user's own dump (`bios/gba_bios.bin`); see
  `bios/README.md` for hash/policy.
- The runtime loads the BIOS, refuses to start if the hash is
  unrecognized, and dispatches every PC < `0x4000` through the
  BIOS dispatch table emitted by `gba_recompile --bios`.
- SWIs land at `0x00000008` and the recompiled BIOS code handles
  them. There is no `if (swi == 0x05) hle_vblank_wait()` hand-written
  stub — the recompiled BIOS bytes are the SWI implementation. (A
  missed BIOS handler is bridged + self-healed, never stubbed.)
- IRQs land at `0x00000018` and the recompiled BIOS IRQ dispatcher
  does its real work: read `IF`, route to the registered handler at
  `0x03007FFC`, return through the real exception epilogue.
- The Nintendo logo check in the BIOS runs against the cartridge's
  logo bytes at `0x08000004..0x080000A0`. We do not bypass it.

**Why:** Stubbed SWIs and skipped BIOS intros are "HLE-by-accident" —
a hand-written guess with no recompiled truth beneath it. That stays
forbidden. Every quirk a real GBA game depends on — timing of
BIOS-driven DMA, exact behavior of `CpuFastSet` on misaligned buffers,
the specific cycle cost of an SWI handler — must be correct *by
construction* in the recompiled bytes. A *silent, permanent*
interpreter would hide IrOp-lowering bugs until cut-over and stays
forbidden; a *loud, self-healing* bridge (interpret once, recompile on
the fly, log + seed the next build) keeps the user playing while we
fix discovery/codegen at the source. The recompiled bytes are always
the eventual truth — we may compile them late, but never quietly.

**How to apply:**
- The runtime exec loop calls `runtime_dispatch(pc)` for every PC.
  There is no blanket `if (pc < 0x4000) interpreter.step()` branch
  that bypasses recompiled BIOS. A dispatch MISS is bridged by the
  interpreter, recompiled on the fly, and logged (see "Honest
  self-healing"); the miss is still a discovery/codegen gap to fix at
  the tool level so the next build needs no bridge.
- Adding a new SWI behavior to the runtime is forbidden. The
  behavior is in the recompiled BIOS bytes.
- If something looks wrong "around an SWI," the divergence is in
  the codegen / runtime ABI / bus / IRQ scheduler — not in the BIOS.
  Fix the recompiler infrastructure, never the BIOS image.
- Symbol annotations from public BIOS disassemblies (see
  `docs/GBA_REFERENCE_NOTES.md` § "GBA BIOS references") are
  reference material only; the BIOS bytes themselves are the
  authoritative behavior.

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
   and honestly reported: logged on first occurrence, counted, dumped
   in a coverage report at exit, and queryable live. We must never be
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
- A hand-written HLE model of guest behavior on the load-bearing path
  (a separate ban — see "Verified-enhancement HLE").

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

## Verified-enhancement HLE is permitted; load-bearing HLE is not

The SHOWSTOPPER rules above forbid hand-written **HLE** on the
load-bearing path — where the program's observable behavior would
depend on a hand-written model being correct — and require that any
interpreter *bridging* self-heal to static and be reported honestly.
That discipline is the heart of this project. (Note: a self-healing
interpreter bridge is NOT HLE — it runs the real guest bytes, just
late; the ban here is on hand-written *models* of guest behavior.)

There is exactly one permitted form of HLE, and it is a quality-of-
life layer that sits **on top of** correctness, never instead of it.
A high-level shadow of a guest subsystem (e.g. an audio-engine
re-render) may ship only if **all** of the following hold:

1. **The recompiled guest code still runs to completion** and remains
   the authoritative output. The shadow never replaces execution.
2. **The canon (recompiled) output stays the verify oracle.** Frame
   hashes, `verify`, and the differential sweeps are defined on the
   recompiled output, never on the shadow.
3. **The shadow is continuously, differentially checked** against the
   canon stream and substitutes only after a proven window.
4. **It reverts loudly** (logs `DEGRADED`) the instant it stops
   matching — never a silent guess.
5. **It is opt-in and present-time**, off by default, and removing it
   changes nothing the recompiler is responsible for.

**Why this is allowed where BIOS-HLE is not:** the forbidden kind has
no canon underneath it — the HLE *is* the behavior, unchecked. A
verified shadow always has the recompiled truth running beneath it
and policing it; its worst failure is "you hear the stock hardware
output," not "the game is subtly wrong." It cannot mask a recompiler
bug, because the recompiled path it shadows is still the thing being
hashed and diffed. This is the same always-on differential-oracle
discipline from the Debug Loop, applied to an enhancement.

**Test before adding any HLE:** "If my reimplementation is wrong,
what happens?" If the answer is "the game misbehaves / a recompiler
bug stays hidden" → forbidden. If it is "we fall back to the
recompiled output and log it" → permitted under this rule.

The current instance: the MP2K audio shadow mixer (`src/runtime`
shadow + `src/gba/gba_m4a`), gated behind the `shadow::Verifier`
differential self-check. Ported from JRickey/gba-recomp.

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
  The recompiled BIOS code handles it via `runtime_dispatch`. There
  is no hand-written HLE path; a missed handler self-heals (see "BIOS
  is sacred — recompiled and dispatched" and "Honest self-healing"
  above).
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
