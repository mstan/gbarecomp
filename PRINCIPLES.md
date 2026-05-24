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

## BIOS is sacred — and recompiled, not interpreted (SHOWSTOPPER)

The GBA BIOS is **recompiled and executed via the dispatch table**,
not stubbed, not HLE'd, not "fast-forwarded," and **not interpreted
on any runtime hot path**. Every game boot path on real hardware
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
  them. There is no `if (swi == 0x05) hle_vblank_wait()`, and there
  is no `interpreter.step(swi_handler_pc)` fallback either.
- IRQs land at `0x00000018` and the recompiled BIOS IRQ dispatcher
  does its real work: read `IF`, route to the registered handler at
  `0x03007FFC`, return through the real exception epilogue.
- The Nintendo logo check in the BIOS runs against the cartridge's
  logo bytes at `0x08000004..0x080000A0`. We do not bypass it.

**Why:** Stubbed SWIs and skipped BIOS intros are "HLE-by-accident."
**A load-bearing interpreter is the same problem in disguise.**
Every quirk a real GBA game depends on — timing of BIOS-driven DMA,
exact behavior of `CpuFastSet` on misaligned buffers, the specific
cycle cost of an SWI handler — must be correct *by construction* in
the recompiler. If the interpreter is propping up correctness on the
hot path, every IrOp lowering bug is invisible until the day we cut
over, at which point the entire dependency stack collapses at once.
We refuse to incur that debt. PSXRecomp / snesrecomp use the same
model.

**How to apply:**
- The runtime exec loop calls `runtime_dispatch(pc)` for every PC.
  There is no `if (pc < 0x4000) interpreter.step()` branch, ever.
  There is no `if (pc not in dispatch table) interpreter.step()`
  fallback either — a missing function is a P0 bug to be fixed in
  discovery / codegen, not papered over.
- Adding a new SWI behavior to the runtime is forbidden. The
  behavior is in the recompiled BIOS bytes.
- If something looks wrong "around an SWI," the divergence is in
  the codegen / runtime ABI / bus / IRQ scheduler — not in the BIOS.
  Fix the recompiler infrastructure, never the BIOS image.
- Symbol annotations from public BIOS disassemblies (see
  `docs/GBA_REFERENCE_NOTES.md` § "GBA BIOS references") are
  reference material only; the BIOS bytes themselves are the
  authoritative behavior.

## Interpreter is informative, never load-bearing (SHOWSTOPPER)

The `armv4t::Interpreter` exists in this tree for **one purpose**:
serve as an offline oracle that we can diff the recompiled output
against. It is debug reference material, never runtime truth.

**INFORMED ✅** — Things the interpreter is good for, and which we
do use:
- Decode trace dumps for a recompiler-vs-interpreter diff harness.
- Per-IrOp semantic reference when writing or fixing codegen.
- `armv4t_tests` unit-test harness for instruction semantics.
- Stand-alone tools (e.g., `interpreter_smoke`, `bios_smoke` in
  oracle-only mode) used to verify the recompiler.

**RELIED-UPON ❌** — Things that mean the recompiler is broken and
must be fixed before any further work:
- Any runtime executable (`MinishCapRecomp.exe`, future games, even
  `bios_smoke` when used as a runner) calling `Interpreter::step`
  on the live CPU state during normal execution.
- Any "fall back to interpreter on unrecognized op" branch.
- Any "interpret BIOS for now, recomp it later" milestone.
- Any "hybrid execution" runtime where some PCs are recompiled and
  others are interpreted.
- Linking `gbarecomp_armv4t` (or any interpreter symbol) into a
  game-runner binary outside diff-oracle tooling.

**Why this is a showstopper, not a preference:** A load-bearing
interpreter silently props up the recompiler's correctness. Bugs
in IrOp codegen, register allocation, flag updates, exception entry
— none of them surface as long as the interpreter is filling in.
The day the interpreter comes out, every one of those latent bugs
fires at once and we can't disentangle them. We pay the cost up
front, instance by instance, while the bugs are diff-isolated and
fixable. This is the recompiler-discipline rule applied to its
hardest case.

**How to apply:**
- The runtime's `run_game()` exec loop never calls into the
  interpreter. Period.
- If the recompiler can't lower an IrOp yet, the generated function
  calls `runtime_unimplemented_op(...)` which aborts with a clear
  PC + opcode. That abort is a P0 bug, addressed by writing the
  lowering, not by routing to the interpreter.
- New diff harnesses are encouraged: any tool that runs the
  recompiler and the interpreter in parallel and diffs the result
  is good engineering. Any tool that runs one as a fallback for the
  other is forbidden.
- This rule applies to **every** console in this ecosystem (NES,
  SNES, PSX, Genesis, Xbox-HLE, future). The interpreter is an
  oracle, never an engine.

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
  is no HLE path and no interpreter fallback (see "BIOS is sacred —
  and recompiled, not interpreted" and "Interpreter is informative,
  never load-bearing" above).
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
