# SLJIT.md — self-contained in-process JIT producer for gbarecomp self-heal shards

Status: **design / not started.** Branch `feat/sljit-backend` (worktree
`gbarecomp-wt-sljit`, off `main` @ `d93d5b1`). This doc specifies adding a
**second code producer** — an in-process JIT on [sljit] — to the existing
Stage-2 self-healing recompiler. It does **not** change the discovery /
dispatch / coverage-honesty model; it adds a toolchain-less way to *produce*
the native shard that today only `g++` can produce.

Companion docs (authoritative, unchanged by this work):
`PRINCIPLES.md` (honest self-healing, coverage honesty, interpreter never
load-bearing), `docs/ROADMAP.md` (Stage 0–4), `docs/ARCHITECTURE.md`,
`docs/TOML_SCHEMA.md`. Sibling-project precedent: `../psxrecomp/SLJIT.md`
(MIPS, shipped + live-validated) and `../psxrecomp/SLJIT_PERSIST_CACHE.md`
(serialized-blob v2). This doc is the gbarecomp/ARM analog.

---

## 0. TL;DR — what this actually is

gbarecomp **already has** the multi-tier self-improving recompiler. Today the
tiers are:

```
Tier 1  static native      kDispatchTable / kBiosDispatchTable (AOT, gba_recompile)
Tier 2  gcc-DLL shard       overlay_loader.cpp: miss → emit C → g++ → cached .dll → g_healed
Tier 3  interpreter floor   runtime_dispatch_miss(): bridge through armv4t::Interpreter
   +    manifest feedback   recomp_master_misses.toml.frag + recomp_coverage.json → human merge
```

The **only** missing tier from the psxrecomp 4-tier model is the **in-process
JIT shard** — a *toolchain-less producer* of the same Tier-2 artifact. Tier 2
today requires `g++` on the machine; a shipped consumer build has no compiler,
so on a player's box the self-improvement loop falls back to the interpreter
(Tier 3) forever on uncovered paths and contributes no coverage.

**This work adds exactly one thing: an `armv4t::Instr → sljit` emitter and a
producer seam that registers its output into `g_healed` the same way the gcc
producer does.** Everything else (discovery, dispatch, validation, cache,
manifest, coverage honesty) is reused unchanged.

The scope is small *because gbarecomp already paid the two prerequisite costs
that made the psxrecomp port expensive* (see §2).

---

## 1. Locked decisions

- **Goal: shippable, toolchain-less players** (decided with the user
  2026-06-17). The player runs the game with no `g++`, hits an uncovered PC,
  and still gets a validated native shard — produced in-process by sljit,
  sub-millisecond, on the miss path. This is the case Tier-2-as-gcc-DLL
  structurally cannot serve.
- **gcc stays the developer default and the release-quality producer.** sljit
  is the *fallback* and the *self-contained production* producer. `auto`
  resolves to gcc when a real toolchain is reachable, else sljit. Dev can
  force either (`GBARECOMP_HEAL_BACKEND=gcc|sljit|auto`).
- **Consumption is producer-blind.** The on-disk cache and `g_healed`
  dispatch must load *the best artifact that runs on this machine*, regardless
  of who produced it. A **shipped gcc DLL** must load and run on a
  toolchain-less (sljit-production) box — that is precisely how a shipped,
  optimized gcc shard *supersedes* a player's locally-JIT'd sljit shard.
  Separate **consumption priority** (static > gcc shard > sljit shard >
  interp, every machine) from **production backend** (who generates on a miss:
  gcc on a dev box, sljit on a player box).
- **Precision over recall (the safety contract, inherited verbatim).** The
  emitter compiles only what it can prove it lowers identically to the
  interpreter. On *any* unsupported op/shape it aborts the **whole function**
  and returns "declined" → the existing Tier-2 gcc path or the Tier-3
  interpreter handles it. A partial emitter is therefore always safe: it can
  decline, never mis-compile. A mis-compiled shard is *fatal* (silent wrong
  state); a declined function is *free* (interp bridges it, exactly as today).
- **The interpreter stays the floor and the oracle.** `armv4t::Interpreter`
  is never on the hot path; it is the semantic reference the sljit emitter is
  validated against (L1 harness, §6) and the runtime same-state differential
  gate (§7). This is the existing PRINCIPLES.md doctrine, unchanged.
- **Coverage honesty unchanged.** An sljit-healed PC is still NOT "fully
  static." It is logged, recorded, and seeds the TOML proposal exactly like a
  gcc-healed PC. `recomp_coverage.json` reports producer counts.

---

## 2. Why gbarecomp is *better* positioned than psxrecomp was

psxrecomp's `SLJIT.md` §5.1 names its dominant cost plainly: *"psxrecomp's
recompiler emits C text … there is no MIPS→native codegen in the project
today. An sljit backend is therefore a whole new codegen path,"* and it
recommends — as the expensive-but-right prerequisite — building a *generator
abstraction* (shared decode/IR core + thin per-instruction leaf emitters) so
the C emitter and the sljit emitter parallel each other and stay in lockstep
forever.

**gbarecomp already has that abstraction, plus three more enablers psxrecomp
had to build from scratch:**

1. **A real, mode-normalized IR + a codegen factored as a per-instruction
   leaf.** ARM and THUMB both decode into one `armv4t::Instr` / `enum class
   IrOp` (29 ops; `src/armv4t/arm_ir.h`). The C codegen is already a thin
   per-instruction leaf — `ArmCodegen::emit_instr(const Instr&, const
   CodegenCtx&, bool* not_implemented)` (`src/armv4t/arm_codegen.h:62`). The
   sljit emitter is a **third consumer of the same `Instr` stream**:
   `emit_instr_sljit(sljit_compiler*, const Instr&, const SljitCtx&, bool*
   declined)`. Divergence between the C path and the JIT path is structurally
   confined to the leaf — the §5.1 abstraction psxrecomp had to *introduce*
   already exists here. THUMB falls out for free: it is normalized to `Instr`
   before the emitter runs, so one emitter covers both ISAs (psxrecomp is
   fixed-width MIPS; gbarecomp gets ARM+THUMB from one emitter).

2. **An interpreter that already parallels codegen semantics, with a per-op
   differential harness — *including cycle parity*.** `tests/codegen/
   test_main.cpp` runs `armv4t::Interpreter::step` and the generated function
   from the identical snapshot and diffs `R[0..14]`, `R[15]` (on branch),
   `CPSR`, full memory, branch side-effects, **and total ticked cycles**
   (`test_main.cpp:291`, `g_ticked_cycles != interp_cycles`). This is the L1
   gate psxrecomp had to design and stand up; here it exists and is green. The
   sljit emitter plugs straight in: make `kTestFns[i]` a JIT-compiled function
   instead of statically-compiled C, and the *same* harness validates the
   sljit emitter op-by-op including cycles (§6).

3. **A position-independent, per-process indirection table — already built.**
   The single enabler psxrecomp's persist-cache (`SLJIT_PERSIST_CACHE.md`,
   §5.4) had to design — *emit every call out of JIT code as an indirect call
   through one per-process dispatch table so the blob is position-independent*
   — gbarecomp already has as `GbaOverlayCallbacks` (`src/runtime/
   overlay_abi.h`): a struct holding `ArmCpuState* cpu` plus one function
   pointer per bus/shifter/flag/dispatch/timing/exception/instrumentation ABI
   symbol generated code may call, ABI-versioned by `GBA_OVERLAY_ABI_VERSION`.
   The gcc DLL path already reaches *all* host state through it (it deliberately
   avoids importing `g_cpu` as a data symbol). The sljit shard emits indirect
   calls through the *same* table — instant ABI-compatibility with the DLL path
   and a free path to serialized blobs (§8 v2).

4. **The whole Tier-2 spine — discovery, async producer seam, content-keyed
   cache, warm-load, third dispatch tier, coverage manifest — is built and
   load-bearing today.** psxrecomp's "generic reusable spine" (~2700 lines)
   maps onto gbarecomp's `overlay_loader.cpp` + `overlay_compile.*` +
   `overlay_emit.*` + `self_heal.*` + the cycle-indexed fingerprint diff.

Net: in psxrecomp, the sljit work was *(port the spine) + (write a net-new
MIPS decoder paralleling a separate interpreter) + (write the emitter) +
(design a position-independent ABI)*. In gbarecomp it is **just the emitter**,
validated by a harness and dispatched through an ABI that already exist.

---

## 3. Tier model (gbarecomp framing)

```
Tier 1  STATIC NATIVE   — AOT-recompiled corpus. kDispatchTable (cart, pc≥0x4000)
                          + kBiosDispatchTable (pc<0x4000). Binary-searched in
                          runtime_dispatch (src/armv4t/runtime_arm.cpp). Fastest.
                          Unchanged.

Tier 2  HEAL SHARD      — produced the first time a PC would otherwise fall to
                          the interpreter. Registered in g_healed so the NEXT
                          encounter is native, persisted under recomp_cache/
                          <image_sha1>/, warm-loaded next session.
       2a gcc DLL         async worker → emit C (overlay_emit) → g++ → .dll →
                          LoadLibrary → overlay_init(callbacks) → func_<pc>.
                          Optimized, inspectable. DEV DEFAULT + release producer.
       2b sljit shard     NEW. sync, in-process, sub-ms, on the miss path. No
                          toolchain. PLAYER-BOX PRODUCER. Same g_healed entry,
                          same GbaOverlayCallbacks ABI.

Tier 3  INTERP FLOOR    — runtime_dispatch_miss() bridges the missed call
                          through armv4t::Interpreter to the stop address, loudly
                          logged + recorded. Correctness floor for not-yet-healed
                          code. Unchanged.

        MANIFEST        — recomp_master_misses.toml.frag ([[extra_func]] /
                          [[jump_table]]) + recomp_coverage.json. Human-reviewed
                          merge into game.toml → regenerate → static. Unchanged.
```

**Per-miss production chain:** active producer (gcc *or* sljit) → if it
declines/fails → interpreter (always the floor). A function gcc can't reach in
time, or that sljit declines, still bridges through Tier 3 and re-attempts —
no producer failure is ever fatal.

**Consumption (dispatch) priority, every machine:** Tier 1 → a *valid* gcc
shard for this PC → a *valid* sljit shard → Tier 3. gcc outranks sljit for the
same content (optimized, dev-validated); a shipped gcc DLL therefore obsoletes
a player's local sljit shard for that PC.

---

## 4. What does NOT change (the producer-agnostic spine)

All of this is reused verbatim; the sljit producer is *only* a code source.

- **Discovery.** `function_finder` bounds the function extent from the entry
  PC; `emit_overlay_c` / the shared `emit_function_body_str`
  (`src/recompile/emit_function.h`) walk that `Instr` list. The sljit emitter
  walks the **same** list over the **same** extent.
- **Dispatch tiering.** `overlay_try_dispatch(pc, thumb)`
  (`src/runtime/overlay_loader.h:38`) is the third tier, called from
  `runtime_dispatch` after the static tables miss and before
  `runtime_dispatch_miss`. An sljit shard lands in `g_healed` and is reached
  by this exact path — **no new dispatch tier is needed.**
- **The miss hook.** `runtime_dispatch_miss`
  (`src/runtime/runtime_arm_default_aborts.cpp`) records the miss and calls
  `overlay_request_compile(pc, thumb)`. For sljit this becomes a *synchronous*
  produce-on-miss (sub-ms) instead of an async enqueue (§5).
- **Content-keyed cache + warm-load + ABI gating.** `recomp_cache/
  <image_sha1>/`, CRC32 of `[pc, end)`, `GBA_OVERLAY_ABI_VERSION` rejection of
  stale artifacts. (Namespacing extension in §9.)
- **Coverage honesty.** `self_heal_*` bookkeeping, the exit banner,
  `recomp_master_misses.toml.frag`, `recomp_coverage.json`. An sljit heal is
  reported with the same honesty as a gcc heal; `recomp_coverage.json` gains a
  per-producer split.
- **The L1 differential harness** (`tests/codegen/`) and the **runtime
  cycle-indexed fingerprint diff** — the validation methodology is unchanged;
  sljit output is gated through both (§6, §7).

---

## 5. The producer seam (`HealProducer`)

Today `runtime_dispatch_miss → overlay_request_compile → [worker thread] →
overlay_compile_one → g_healed`. `overlay_compile_one`
(`src/runtime/overlay_compile.h:52`) is the gcc producer: it takes an
`OverlayWorkItem { pc, thumb, bytes, size, base }`, discovers the extent,
emits C, runs `g++`, `LoadLibrary`s, ABI-gates, `overlay_init`s, resolves
`func_<pc>`, and fills an `OverlayCompiled { pc, thumb, crc, end, module, fn }`.

Introduce a thin producer interface so the loader is no longer hard-wired to
g++. This mirrors psxrecomp's `CodeProvider` but is lighter, because the
loader already owns discovery, cache, queues, and `g_healed`:

```c
// who can produce a native shard for one OverlayWorkItem
typedef enum { HEAL_GCC, HEAL_SLJIT } HealBackend;

struct HealProducer {
    const char* name;                 // "gcc" | "sljit"
    int  (*available)(void);          // gcc: real toolchain on PATH; sljit: always 1
    // Produce a native function for w into *out. Return 1 on success (out->fn
    // set), 0 on decline/failure (caller stays on the interpreter bridge).
    int  (*produce)(const OverlayWorkItem* w, const char* cache_dir,
                    const GbaOverlayCallbacks* cb, int allow_build,
                    OverlayCompiled* out, char* err, size_t errlen);
    int  synchronous;                 // gcc: 0 (async worker); sljit: 1 (on-miss)
};
```

- **gcc producer** = today's `overlay_compile_one` behind this interface
  (pure refactor; byte-identical behavior; `synchronous = 0`, keeps the worker
  thread + `overlay_drain_ready` flow).
- **sljit producer** = new (`src/runtime/overlay_sljit.{h,c}`):
  `synchronous = 1`. On a miss it discovers the extent (same `function_finder`
  call `overlay_emit` uses), JITs in-process (§5.1), and fills `out->fn`
  directly — no worker, no `.dll`, no rescan latency. Declines → `return 0`.
- **Resolution.** `heal_backend_resolve()` reads `GBARECOMP_HEAL_BACKEND`
  (env) > `[runtime] heal_backend` (config) > `auto`. `auto` →
  `gcc_toolchain_available()` ? gcc : sljit. The gcc-availability probe must
  test a **reachable compiler on PATH**, not merely that a command string is
  configured (the shipped `game.toml` may carry a `heal_compile_cmd` that does
  not exist on a player box). Resolved once at init, logged.
- **Complementary, not exclusive (optional, later).** sljit may also be made
  available *alongside* gcc to fill the async gap: on a dev box, while a gcc
  DLL is still compiling on the worker, an sljit shard can carry the PC live
  (validated) instead of bridging through the interpreter, and is retired when
  the optimized gcc DLL lands (Phase B obsolescence, §9). Off by default until
  the gcc-default path is proven; the simplest first cut is one active
  producer.

### 5.1 In-process JIT shape

The unit is **one discovered function** (`function_finder` extent), matching
the gcc overlay. The emitted artifact is a `void fn(void)` that operates on
host state through `GbaOverlayCallbacks`, exactly like the DLL's `func_<pc>`:

- **Entry.** A tiny fixed trampoline supplies the callbacks pointer to the JIT
  body (`fn()` closes over `&g_overlay_callbacks`; the body loads
  `cpu = cb->cpu` into a saved register once). For v1 (re-JIT each session)
  the callbacks pointer may be baked as an immediate; v2 serialization reaches
  it cpu-relative (§8).
- **GPRs.** v1: memory-backed — every operand is a load/store at
  `cpu->R[n]` (the model the interpreter and the generated C both use,
  trivially parity-correct). A later optimization caches block-local GPRs in
  host saved registers across the call-free spans (psxrecomp commit `b001e03`;
  flush before every control transfer and before any callback that reads
  `cpu->R[]`, reset at branch-target joins, never cache `R[15]` mid-flow).
  **Defer until the memory-backed emitter is differential-clean.**
- **ALU / shifter / flags.** Emit straight sljit ops where the result is
  provably identical (add/sub/and/or/eor/mov/bic/mvn, immediate and
  register-shifted operands), and call `cb->arm_set_nz* / arm_shift_*` for the
  flag/shifter semantics that the C path already funnels through those helpers
  — guaranteeing bit-identical NZCV without re-deriving carry/overflow in the
  emitter. Helper-emitted ops are *parity-by-construction* (the helper IS the
  reference).
- **Loads / stores.** Indirect call through `cb->bus_read_* / bus_write_*` —
  same MMIO/watch path as the interpreter and the DLL.
- **Intra-function control flow.** Two-pass, as psxrecomp does: pass 1 scans
  the extent, collects branch targets, validates them (in-range, mode-correct,
  not mid-instruction); pass 2 emits an sljit label per target and binds
  conditional/unconditional jumps. ARM condition codes wrap the block in the
  `cb->arm_cond_passes(cond)` predicate the C path already uses.
- **Inter-function flow = tail back to the host.** gbarecomp already routes
  *all* overlay inter-function flow through the dispatcher (overlays are
  emitted with an empty `names_by_key`, so every direct B/BL lowers to
  `runtime_dispatch(target)`; see `emit_function.h`). So the sljit shard emits
  function calls / returns / `BX`/`BLX` as indirect calls to
  `cb->runtime_dispatch` / `cb->runtime_dispatch_with_exchange` /
  `cb->runtime_call_push_return` / `cb->runtime_call_should_return`, then
  returns from `fn`. This is *simpler* than psxrecomp's jal/jalr/jr-table work
  because the funnel already exists.
- **Cycle accounting (hard requirement).** The generated C calls
  `runtime_tick(n)` with `n` from `instr_cycle_base(op)` plus
  `cb->runtime_mem_cycles(...)` (N/S waitstates) and
  `cb->runtime_mul_cycles(...)` (operand-dependent multiply waits). The sljit
  shard **must emit the identical `runtime_tick` calls** — bake
  `instr_cycle_base(op)` as an immediate per instruction and call the same
  `cb->runtime_mem_cycles / runtime_mul_cycles` for the variable parts. The L1
  harness already fails on a one-cycle delta (`test_main.cpp:291`), and the MC
  history (banked-SP / uncounted IRQ-wake cycles) shows cycle drift causes real
  PPU/frame divergence. **Cycle parity is part of "lowers identically," not an
  afterthought.**

### 5.2 Decline criteria (abort whole function → 0)

Any of: an `IrOp` the emitter does not yet lower; an operand shape it can't
prove (e.g. an unhandled shifter form); a branch target out of range /
mode-mismatched / mid-instruction; `function_finder` returns no entry;
function exceeds a size/branch cap; any `MSR/MRS/SWI/coprocessor/LDM-with-PC`
case not yet covered. Declining is *correct and cheap* — the gcc path or the
interpreter handles it. Start with a deliberately narrow op set and widen
under the harness (§6, §10).

---

## 6. Validation gate L1 — the per-op differential (offline, deterministic)

The existing `tests/codegen` harness is the primary proof and needs only a
*producer toggle*:

- Today `gen_codegen_tests.cpp` emits one C function per `kTestCases` entry;
  `codegen_tests` links them as `kTestFns[i]` and `test_main.cpp` diffs each
  against `armv4t::Interpreter::step` (registers, CPSR, memory, branch
  side-effects, **cycles**).
- Add a **JIT mode**: a `kTestFns[i]` whose body is produced by
  `emit_instr_sljit` (JIT the single decoded `Instr`, run it over the same
  `g_cpu` + stub bus). The diff machinery, the stub bus
  (`codegen_test::bus_*`), the `g_ticked_cycles` check, and the
  `g_unimplemented_called` abort check are reused **unchanged**. A new
  `sljit_codegen_tests` target (or a runtime flag on the existing one) runs the
  identical corpus through the JIT.
- Plus a standalone emitter unit test (`tests/codegen/sljit_emit_test.c`,
  links only the emitter + `sljitLir.c`) that JITs hand-built functions and
  checks results — including every *decline* path (proves "declines, never
  mis-compiles"), mirroring psxrecomp's `sljit_emit_test.c` (66/66).
- **Gate:** an `IrOp` is "supported by sljit" only when its `kTestCases`
  entries pass the JIT-mode diff *including cycles*. The supported set is
  exactly the differential-clean set; the emitter declines everything else.

This is the L1 gate, and it is **stronger than psxrecomp's** at the same stage
because it checks cycle parity, which gbarecomp's frame-exactness requires.

---

## 7. Validation gate L2 — the runtime same-state differential (in-vivo)

Before an sljit shard is trusted to run live in normal play, validate it
against the interpreter from identical state, reusing gbarecomp's existing
runtime diff machinery (the cycle-indexed per-instruction fingerprint ring +
the phase-aligned IWRAM/EWRAM diff used for MC-HP work):

- On the first heal of a PC, run the shard and the interpreter bridge from the
  same pre-call snapshot, compare post-state (registers + the function's RAM
  writes + ticked cycles + the fingerprint stream). Keep the interpreter
  result; the shard's pass only *promotes* it.
- **Consecutive-clean budget.** Promote to live-without-diff only after N
  consecutive 0-divergence passes; reset the counter to 0 on *any* divergence
  (so an intermittently-wrong shard never reaches budget on lucky passes — the
  bug psxrecomp's Phase A fixed). A diverging shard stays diff-gated forever
  and never runs blind.
- **Device/MMIO functions** (those whose interpreter pass touches IO
  registers) are pinned to the interpreter and never run as a shard, to avoid
  double-executing side-effectful IO during the diff (psxrecomp's
  `device_touch` skip).
- Gated behind the heal flag; off in shadow/oracle runs so normal verification
  stays byte-identical.

`GBARECOMP_SELFHEAL_RECOMPILE` (the existing master gate) plus a
`GBARECOMP_HEAL_SLJIT_LIVE` override govern this, defaulting live-on only when
the resolved producer is sljit (a player box self-improves on the normal play
path) and only *after* L1 is green.

---

## 8. Persistence

Requirement #3 (coverage survives the session) is already met for gcc by the
on-disk DLL cache + warm-load. For sljit:

- **v1 (ship this): re-JIT from the coverage manifest on launch.** The
  portable currency is the manifest (`recomp_coverage.json` + the merged
  `game.toml` seeds), not native bytes. Re-JITting a function is sub-ms, so a
  warm start re-derives every sljit shard from the seed set effectively for
  free. No `sljit/` blob dir needed. A function is discovered-through-the-
  interpreter *at most once ever*, because the seed persists.
- **v2 (only if launch re-JIT ever becomes measurable): serialized blobs.**
  `sljit_serialize_compiler` before `generate_code`, persisted under
  `recomp_cache/<image_sha1>/sljit/<arch-abi>/cg<ver>/<pc>_<crc>.sljit`,
  reloaded via `sljit_deserialize_compiler`. This is tractable here precisely
  because all external references already go through `GbaOverlayCallbacks`
  (position-independent except one cpu-relative table base), so a blob needs
  no thousands-of-relocations rebind table — psxrecomp proved this in
  `SLJIT_PERSIST_CACHE.md`. Defer until v1's startup cost is shown to matter.

JIT code must **never** be persisted as raw host bytes with baked absolute
pointers (ASLR / different exe → wrong). v1 sidesteps this entirely by
re-JITting; v2 sidesteps it with the callbacks indirection.

---

## 9. Cache namespacing — no comingling

Today's cache is flat: `recomp_cache/<image_sha1>/<pc>_<crc>_<a|t>.dll`. Before
sljit lands (lowest-risk, do first), split by producer **and** target so a
Windows-x64 gcc DLL and a future Linux-arm64 sljit blob for the same function
never collide and a stale artifact never wins:

```
recomp_cache/<image_sha1>/
  coverage/                         portable, contributable (the merge unit)
  gcc/<os-arch>/cg<ver>/<pc>_<crc>_<a|t>.dll
  sljit/<os-arch>/cg<ver>/<pc>_<crc>_<a|t>.sljit   (v2 only)
```

Rules: **consumption is producer-blind** (load the best artifact that runs on
this `<os-arch>`, scanning *both* `gcc/` and `sljit/`); `cg<ver>` invalidates
stale blobs when the emitter changes; warm-load keeps scanning the legacy flat
path additively (no migration; existing caches keep loading). **Obsolescence:**
when a gcc DLL registers for a PC whose live bytes match an sljit shard of the
same content, retire the sljit shard (blacklist) — gcc deterministically
supersedes sljit; the two never co-execute.

---

## 10. Incremental plan (additive; never disturb the validated gcc path)

Each phase is independently shippable; the gcc-DLL path stays the default and
byte-identical until the final gate.

- **P0 — cache namespacing** (`coverage/` + `gcc/<os-arch>/`). No behavior
  change; prevents a future comingled cache. *Do first.*
- **P1 — `HealProducer` seam.** Refactor `overlay_compile_one` behind the
  interface; gcc remains the only producer; behavior identical. Add
  `heal_backend_resolve` + the real-toolchain probe + `[runtime] heal_backend`
  config + `GBARECOMP_HEAL_BACKEND` env. Pure refactor.
- **P2 — vendor sljit + smoke.** Copy `lib/sljit/` (BSD-2, from
  `../psxrecomp/lib/sljit`), wire into CMake, add a JIT'd `f(a)=a+1234`
  selftest proving codegen + the executable allocator work on the host.
- **P3 — emitter first slice + L1 JIT mode.** `emit_instr_sljit` for a narrow
  op set (the data-processing core + immediate/register operands + the
  `arm_set_*`/`arm_shift_*` flag helpers + `runtime_tick`), straight-line
  functions terminating in a tail `runtime_dispatch`. Stand up
  `sljit_codegen_tests` (the L1 harness in JIT mode) + `sljit_emit_test.c`.
  Gate each op on a clean diff *including cycles*.
- **P4 — widen the emitter.** Loads/stores (via `bus_*`), intra-function
  branches (two-pass labels + `arm_cond_passes`), LDM/STM, multiplies (with
  `runtime_mul_cycles`), `BX`/`BLX`/call-return. Each class gated by the
  harness. Track the declined set in `recomp_coverage.json`.
- **P5 — wire the sljit producer on-miss** (synchronous), register into
  `g_healed`, persist the seed (manifest), warm re-JIT on launch (§8 v1).
- **P6 — L2 runtime differential gate** (§7): consecutive-clean budget,
  device-touch skip, promote-then-run-live, gcc>sljit obsolescence.
- **P7 — production trigger.** `auto` → sljit on a toolchain-less box;
  validated-live default-on when the resolved producer is sljit.

**Promotion to default** is gated on: L1 green across the supported op set
(cycles included), L2 broad diff-clean across MinishCap play (0 divergences, 0
wedges, device fns skipped, warm re-JIT closes the loop), and user sign-off.
gcc stays the dev default throughout.

---

## 11. GBA/ARM-specific considerations

- **THUMB is free** (normalized to `Instr` pre-emitter) — one emitter, both
  ISAs. A net advantage over the MIPS port.
- **Cycle-exactness is the hard constraint** (§5.1, §6). gbarecomp is
  frame-exact and the harness enforces per-instruction cycle parity; the
  emitter must reproduce `instr_cycle_base` + the `runtime_mem_cycles` /
  `runtime_mul_cycles` waits exactly.
- **Banked registers / mode changes** (`MSR`/`MRS`/`SWI`/IRQ/exception
  return). The C path funnels these through `runtime_msr_* / runtime_mrs_* /
  runtime_swi / runtime_irq / runtime_exception_return /
  runtime_restore_cpsr_from_spsr` (all in `GbaOverlayCallbacks`). The sljit
  shard calls the same helpers (parity-by-construction) or **declines**
  functions containing them until explicitly covered. Banked-SP correctness
  bugs have bitten this project before — prefer declining to guessing.
- **Self-modifying / dynamic-RAM code (IWRAM/EWRAM)** remains **Stage 4 / out
  of scope**, exactly as for the gcc producer (`overlay_loader` resolves only
  BIOS + ROM immutable images). The sljit producer inherits the same region
  restriction; do not expand surface area here (constrain-surface-area
  doctrine).
- **BIOS region (pc < 0x4000)** heals identically (the loader already supplies
  the 16 KB BIOS snapshot as the code image); sljit shards for BIOS PCs work
  the same way, subject to the same decline rules.

---

## 12. Open questions / risks

1. **Emitter↔C parity, forever (dominant risk).** Two producers must compute
   identically *and* tick identically. Mitigation: the shared `Instr` leaf
   structure (§2.1), the L1 cycle-checked harness as a mandatory gate (§6), and
   precision-over-recall (a mis-compile is fatal, a decline is free). Every
   recompiler change that touches lowering or cycles must keep both green.
2. **sljit runtime perf < gcc.** Baseline JIT, no real optimization. Framing:
   production is not all-sljit — the release ships *gcc-optimized* shards for
   accumulated community coverage; sljit carries only a given player's
   not-yet-folded long tail, and is still vastly faster than the interpreter
   (the actual status-quo alternative for a toolchain-less user). Memory-backed
   GPRs first; block-local register cache (§5.1) only if a hot shard needs it.
3. **Cycle parity in the JIT is more work than in C** (C just calls
   `runtime_tick`; the JIT must emit those calls with the right immediates).
   Bounded and harness-checked, but it is the part most likely to leak bugs —
   treat it as first-class in P3.
4. **Arch coverage.** sljit is solid on x86-64; ARM64 (Apple-Silicon players)
   less battle-tested. Per-arch cache namespacing (§9) contains the blast
   radius; v1 re-JIT means no cross-arch blob portability is even attempted.
5. **Two-producer test matrix.** CI must run the L1 harness under both
   producers and the L2 differential under sljit.

---

## 13. Scope estimate

| Piece | New? | Rough size | Notes |
|---|---|---|---|
| Cache namespacing (P0) | refactor | ~150 ln | path scheme only |
| `HealProducer` seam (P1) | refactor | ~200 ln | wrap `overlay_compile_one` |
| Vendor sljit + CMake + smoke (P2) | copy | ~vendor | from `../psxrecomp/lib/sljit` |
| **`emit_instr_sljit` (P3–P4)** | **new** | **~900–1300 ln** | the one real cost; parallels `arm_codegen.cpp` |
| L1 JIT mode + `sljit_emit_test` (P3) | new | ~300 ln | reuses `test_main.cpp` diff |
| sljit producer + on-miss wiring (P5) | new | ~250 ln | parallels `overlay_compile.c` |
| L2 differential gate (P6–P7) | new/reuse | ~300 ln | reuses fingerprint/IWRAM diff |

The dominant cost is the **one emitter** — and it is gated by a harness and
dispatched through an ABI that already exist. psxrecomp's port was ~2× this
because it had to build the spine, a parallel decoder, and the
position-independent ABI first; gbarecomp does not.

---

## 14. Test target (MinishCap)

MinishCap boots and runs on `main`; it is the L2 differential target. Create a
matching `feat/sljit-backend` branch in `MinishCapRecomp` at implementation
start (P5), bootstrap the worktree per `project_worktree_layout` (copy
tomlpp+bios, build the tool target, regen, reconfigure), and run the
play-through differential there. FireRed is a separate boot problem (static
coverage / identity), orthogonal to this producer work — though turning the
existing Stage-1 interpreter bridge on against FireRed and reading
`recomp_coverage.json` is a good independent diagnostic for *where* it dies.

[sljit]: https://github.com/zherczeg/sljit
