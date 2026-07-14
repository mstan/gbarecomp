// runtime_arm.h — C-ABI surface that generated recompiled cart code
// calls into.
//
// Generated code is .cpp but uses C linkage on these symbols so the
// recompiler doesn't have to worry about C++ name mangling. The
// implementations live in runtime_arm.cpp and delegate to the
// existing C++ runtime (armv4t::CPUState, gba::GbaBus, etc.).
//
// ABI principle: the symbols declared here are the ONLY interface
// between gba_recompile's generated output and the runtime. If new
// codegen needs an operation, declare a helper here and implement
// once — never inline runtime-internal types into generated code.

#pragma once

#include <stdint.h>

// Shared POD types + bit/constant macros (ArmCpuState, CPSR_*_BIT,
// RUNTIME_TRACE_*, RuntimeTraceEntry, RuntimeFpEntry). Split out so the
// Stage-2 overlay shim can reuse the exact same struct layouts without
// pulling in this header's `extern g_cpu` + inline accessors.
#include "runtime_arm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── CPU state ──────────────────────────────────────────────────────
// Generated code reads/writes the register file via this global.
// The CPSR is packed:
//
//   bit 31 N
//   bit 30 Z
//   bit 29 C
//   bit 28 V
//   bit  7 I (IRQ disable)
//   bit  6 F (FIQ disable)
//   bit  5 T (THUMB state)
//   bits 4..0 mode
//
// ArmCpuState + the ARM_BANK_* / CPSR_*_BIT macros are defined in
// runtime_arm_types.h (shared with the overlay shim).

extern ArmCpuState g_cpu;

static inline uint32_t cpsr_n(void) { return (g_cpu.cpsr & CPSR_N_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_z(void) { return (g_cpu.cpsr & CPSR_Z_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_c(void) { return (g_cpu.cpsr & CPSR_C_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_v(void) { return (g_cpu.cpsr & CPSR_V_BIT) ? 1u : 0u; }

// ARM condition-code evaluators — true if the cond passes given
// the current CPSR. cond is the 4-bit code from the instruction
// encoding. AL (1110) is unconditional (true); NV (1111) is "never"
// on ARMv4T.
int arm_cond_passes(unsigned cond);

// ── Bus ────────────────────────────────────────────────────────────
// All cart reads/writes flow through these. The runtime sets up the
// bus pointer before any cart code runs; bus_set_active() is called
// from the runner's main().

uint32_t bus_read_u32(uint32_t addr);
uint16_t bus_read_u16(uint32_t addr);
uint8_t  bus_read_u8 (uint32_t addr);
void     bus_write_u32(uint32_t addr, uint32_t val);
void     bus_write_u16(uint32_t addr, uint16_t val);
void     bus_write_u8 (uint32_t addr, uint8_t  val);

// ── Per-instruction cycle cost (memory + multiply) ─────────────────
// Generated code computes the fixed part of an instruction's cost
// statically (instr_cycle_base + shift/PC-write surcharges) and adds
// these runtime-dependent parts as it executes, then ticks the total
// once at the instruction boundary. `runtime_mem_cycles` returns the
// N/S access cost for the active bus region; `runtime_mul_cycles`
// returns the ARM7TDMI multiply operand wait. Both mirror the IR
// interpreter (the timing oracle) exactly. `width` is 1/2/4 bytes;
// `sequential`/`signed_variant` are 0/1 flags.
uint32_t runtime_mem_cycles(uint32_t addr, uint32_t width,
                            uint32_t sequential);
uint32_t runtime_mul_cycles(uint32_t rs_value, uint32_t signed_variant,
                            uint32_t extra);

// ── Shifter helpers ────────────────────────────────────────────────
// Generated code uses these for data-processing operand2 shifts and
// for register-shifted-by-register cases. They update the shifter
// carry-out into CPSR.C when set_carry != 0.

uint32_t arm_shift_lsl(uint32_t v, uint32_t n, int set_carry);
uint32_t arm_shift_lsr(uint32_t v, uint32_t n, int set_carry);
uint32_t arm_shift_asr(uint32_t v, uint32_t n, int set_carry);
uint32_t arm_shift_ror(uint32_t v, uint32_t n, int set_carry);

// ── Flag updaters ──────────────────────────────────────────────────
// After an arithmetic/logical op with S=1, generated code calls one
// of these to set CPSR.NZ(CV). The C-out / V-out semantics follow
// ARM ARM A4.1 (data processing).

void arm_set_nz   (uint32_t result);
void arm_set_nzc_logic(uint32_t result, uint32_t shifter_carry);
void arm_set_nzcv_add(uint32_t a, uint32_t b, uint32_t result);
void arm_set_nzcv_adc(uint32_t a, uint32_t b, uint32_t c_in, uint32_t result);
void arm_set_nzcv_sub(uint32_t a, uint32_t b, uint32_t result);
void arm_set_nzcv_sbc(uint32_t a, uint32_t b, uint32_t c_in, uint32_t result);

// ── Dispatch ───────────────────────────────────────────────────────
// `target_pc` is a guest PC. Low bit indicates THUMB on BX/BLX. The
// dispatcher binary-searches the per-game dispatch table; if the
// target is found, the corresponding generated function is called.
// Misses route to runtime_dispatch_miss for logging + fallback.

void runtime_dispatch(uint32_t target_pc);
void runtime_dispatch_with_exchange(uint32_t target_pc);
void runtime_dispatch_miss(uint32_t target_pc);

// Whole-program force-interpreter backend (co-simulation "interp" side). When
// g_force_interp != 0, the main run loop calls runtime_force_interp_step() once
// per guest instruction instead of dispatching generated code — interpreting the
// main thread while reusing runtime_tick / runtime_swi for the device/IRQ/BIOS
// path (so both co-sim backends share everything but instruction execution). Set
// from GBARECOMP_FORCE_INTERP at startup. See COSIM_ORACLE.md §1.
extern int g_force_interp;
void runtime_force_interp_step(void);

// True iff a STATIC (recompiled) dispatch-table entry exists for this guest PC
// + instruction-set state. The on-miss bridge uses it to detect re-entry into
// statically recompiled code and hand control back (heal-to-static) when it has
// no reliable stop address (a top-level / exception-return miss).
int runtime_has_static_entry(uint32_t pc, int thumb);

// The interpret-the-missed-subtree core of the on-miss bridge, factored so the
// P6 sljit differential gate can reuse it as its "interpreter pass" (the kept
// result a healed shard is validated against). Interprets from (entry_pc,
// entry_thumb) until control returns to the stop address, mutating g_cpu live
// and ticking each instruction (IRQ self-delivery + SWI routing exactly as in
// normal play). forced_stop_pc=0 → use the call-return-stack contract (the
// on-miss bridge). Non-zero → stop there instead: the gate passes the entry LR,
// since a function reached by a computed jump (not a BL) has no matching
// call-return frame and the stack-top contract would mis-target and run away.
// max_instrs=0 → the default 200M abort-on-runaway; non-zero bounds the walk and
// returns 0 (instead of aborting) if the stop isn't reached in that budget — the
// gate uses this to fall back safely when its forced stop turns out wrong.
// Returns 1 if it stopped cleanly at the stop address, 0 if it hit the budget.
int runtime_bridge_interpret(uint32_t entry_pc, bool entry_thumb,
                             uint32_t forced_stop_pc, uint64_t max_instrs);

// Direct generated BL calls use the host C stack for speed and clarity.
// Return idioms (`bx lr`, `mov pc, lr`, `pop {..., pc}`) are only C
// returns when they match the top direct-call return address; otherwise
// they are real guest branches and must dispatch.
void runtime_call_push_return(uint32_t return_pc);
int  runtime_call_should_return(uint32_t target_pc);
void runtime_call_cancel_return(uint32_t return_pc);

// Save-state support: expose the host-side call-return stack so the
// snapshot orchestrator can serialize/restore it. The stack lives in
// runtime_arm.cpp (file-local); these accessors are the only sanctioned
// window. restore replaces the live stack wholesale (clamped to the
// stack capacity). Returned pointer is valid until the next push/pop.
uint32_t        runtime_call_stack_depth(void);
const uint32_t* runtime_call_stack_data(void);
void            runtime_call_stack_restore(const uint32_t* entries,
                                           uint32_t depth);

// Always-on structured execution trace. The RUNTIME_TRACE_* kind macros
// and RuntimeTraceEntry are defined in runtime_arm_types.h. This records
// diagnostic state only; it never routes execution or substitutes for
// missing codegen.
void runtime_trace_event(uint32_t kind, uint32_t pc, uint32_t addr,
                         uint32_t value, uint32_t aux);
void runtime_trace_reset(void);
void runtime_trace_dump_recent(uint32_t max_entries);

// ── Per-instruction fingerprint ring (MC-HP-002 cycle-aligned diff) ──
// When g_runtime_insn_trace != 0 the generated per-instruction prologue calls
// runtime_insn_fp() to record the pre-execution architectural state of EVERY
// instruction: {cumulative cycles, pc, cpsr, R0..R15}. The ring is always-on
// once armed (armed at machine reset from GBARECOMP_INSN_TRACE, or via the TCP
// `insn_trace` command) and bounded by eviction; a targeted dump pulls the
// window of interest. This is the substrate for diffing the recomp against the
// bios_smoke interp oracle at identical cycle counts — the FIRST fingerprint
// that differs (pc or any register) localizes the first real divergence.
// Disarmed (default) it costs one not-taken branch per instruction.
// RuntimeFpEntry is defined in runtime_arm_types.h (shared with the overlay
// shim).
extern unsigned g_runtime_insn_trace;  // 0 = off (zero overhead)
void runtime_insn_fp(void);            // emit one fingerprint (armed-gated by caller)
void runtime_fp_reset(void);
uint32_t runtime_fp_count(void);
// Write the whole ring (oldest-first) as a compact binary file: a 16-byte
// header {u32 magic 'GFP1', u32 entry_size, u64 count} followed by `count`
// RuntimeFpEntry records. Returns the number of records written (0 on error).
uint32_t runtime_fp_save_file(const char* path);
// Dump the LAST `n` fingerprint records (oldest-first in the window) as CSV
// (idx,cycles,pc,cpsr,r0..r15). The hang watchdog calls this on trip so the
// execution history leading into a freeze is on disk with no live interaction
// (n==0 → whole ring). Requires GBARECOMP_INSN_TRACE armed. Returns records.
uint32_t runtime_fp_save_tail_csv(const char* path, uint32_t n);
// Cycle-anchor sampler (Axis 2 — accuracy burndown): filter the always-on
// insn-fingerprint ring by guest PC, writing up to `max_hits` cumulative
// g_runtime_cycles stamps (oldest-first) for every recorded execution of `pc`
// into `out_cycles`. Returns the number written. The Δ between consecutive hits
// is the offset-cancelled cycle "ruler" peer to the NBA oracle's cyc_anchor.
// Requires GBARECOMP_INSN_TRACE armed (the ring is the only always-on
// per-instruction (pc,cycle) source); returns 0 when the ring is empty.
uint32_t runtime_fp_query_pc(uint32_t pc, uint32_t max_hits,
                             unsigned long long* out_cycles);

// Current recompiled-CPU guest PC (g_cpu.R[15]). Read-only accessor exposed so
// always-on observability taps outside the armv4t lib (e.g. the gba_io MMIO
// write-trace ring) can stamp the originating PC without coupling to the
// ArmCpuState layout. Reflects the recomp register file (meaningful in the
// recompiled runtime; the bios_smoke interpreter drives its own CPUState).
uint32_t runtime_current_pc(void);

// ── Function-entry hook (general debug observability) ──────────────────
// When g_runtime_fn_entry_hook != nullptr the generated function prologue
// calls it with the guest entry PC, BEFORE the first instruction of that
// function runs. At that instant g_cpu.R[0..3] hold the AAPCS arguments and
// g_cpu.R[14] the return address, so a host probe can read a recompiled
// function's arguments and guest memory at call time. This is the general
// substrate for host-side observation of ANY recompiled guest function across
// ANY game (e.g. the widescreen tilemap-provenance probe reads DrawMetatileAt's
// world-coord args). nullptr (the default) costs one not-taken branch per
// function entry and is byte-identical to the un-hooked guest state.
extern void (*g_runtime_fn_entry_hook)(uint32_t entry_pc);

// ── Mid-function alias resume PC ───────────────────────────────────────
// Set by a thin per-alias dispatch wrapper (generated) to the alternate
// entry PC immediately before it tail-calls the host function; the host's
// resume prologue reads it, clears it, and computed-goto's to the interior
// label for that PC. 0 (the default) = normal entry: the host falls through
// the prologue and starts at its first instruction. This is how an IRQ/SWI
// return into the middle of an already-recompiled function (e.g. a
// WaitForVBlank busy-spin) re-enters the WHOLE native function at the right
// point instead of fragmenting it into dispatch-chained pieces.
extern uint32_t g_runtime_resume_pc;

// Dedicated always-on IRQ-vector log (MC-HP-002): one entry per IRQ vectoring,
// dumped as CSV. Armed by env GBARECOMP_IRQ_LOG. g_runtime_irq_from_halt is set
// by runtime_tick's wake-from-HALT path so each log entry records whether the
// vector woke the CPU from HALT.
void runtime_irq_log_record(uint32_t src, uint32_t ret, uint32_t cpsr);
uint32_t runtime_irq_log_save_file(const char* path);
extern uint32_t g_runtime_irq_from_halt;
// Live TCP query peer of the IRQ-vector log (Axis 3 — accuracy burndown). The
// ring records the IRQ TAKE point (vectoring): cycle stamp, active source mask
// (IE & IF), return address, saved CPSR, and the from-HALT flag. Raise-time (the
// IF-set instant) is NOT separately recorded — this is take-time only; that gap
// is noted in the burndown. Recording is always-on (Release too); the env
// GBARECOMP_IRQ_LOG only governs the on-exit CSV dump.
typedef struct RuntimeIrqLogEntry {
    unsigned long long cycles;
    uint32_t src;
    uint32_t ret;
    uint32_t cpsr;
    uint32_t from_halt;
} RuntimeIrqLogEntry;
// Copy the most recent `max` IRQ-vector entries (oldest-first) into `out`.
uint32_t runtime_irq_log_copy_recent(RuntimeIrqLogEntry* out, uint32_t max);
uint32_t runtime_irq_log_count(void);
// SWI log (milestone-PC sequence): every SWI with cycle + caller + r0/r1 +
// BIOS IntrWait flags (0x03007FF8). Armed by env GBARECOMP_SWI_LOG.
void runtime_swi_log_record(uint32_t imm, uint32_t ret, uint32_t r0,
                            uint32_t r1, uint32_t r2, uint32_t lr,
                            uint32_t iwflags);
uint32_t runtime_swi_log_save_file(const char* path);
uint32_t runtime_trace_copy_recent(RuntimeTraceEntry* out,
                                   uint32_t max_entries);
void runtime_tick(uint32_t cycles);
bool runtime_should_yield(void);

// ── Stage 2 idle-loop elision ──────────────────────────────────────
// Emitted by codegen at the back-edge of a statically-eligible quiescent
// loop (CodegenCtx::idle_backedge_pcs), called once per completed iteration
// with the loop-header PC as the site identity. The runtime keeps a per-site
// rolling fixed-point proof: when two consecutive iterations leave {R0-R14,
// full CPSR} identical, span the same cycle period, and no disturbance
// (memory write, MMIO read, IRQ, or serviced device event) occurred between
// them, the loop is provably idle until an external agent changes its watched
// state. It then fast-forwards the Stage-1 master clock by whole loop periods
// to one period before the next scheduled event (g_event_budget horizon) —
// servicing NO event inside the hook — and returns with the CPU positioned at
// the loop header, so the final event-crossing iterations run normally and the
// IRQ is delivered at the exact baseline cycle. Bit-identical by construction.
void runtime_idle_backedge(uint32_t header_pc);

// Monotonic "something that could change a watched poll value or the timing
// rules happened" counter. Bumped on every guest memory write, every MMIO
// read (so MMIO-polling loops never qualify), every IRQ entry, and every
// device event materialization. The idle prover requires it to be unchanged
// across the two proof iterations. (Correctness-first: false invalidations
// only cost a re-prove; a missed bump would be unsound.)
extern unsigned long long g_idle_disturb_epoch;
// Monotonic host-side notification for presentation caches that are not part
// of the serialized GBA state. Incremented after every successful state load.
extern unsigned long long g_runtime_state_epoch;
// Cumulative guest-cycle clock. Incremented by runtime_tick on EVERY tick
// (per-instruction exec ticks AND halt-pump chunks), so it is the authoritative
// total-cycle count — unlike runtime.cpp's `cycles_elapsed`, which only tallied
// the halt path (the MC-HP-002 "cycles incomparable" red herring). Reset to 0
// at machine reset (runtime_trace_reset). Stamped onto every ring entry so the
// recomp and interp oracle can be aligned by identical cycle counts. (MC-HP-002.)
extern unsigned long long g_runtime_cycles;

// P6 sljit differential gate — shadow-tick mode. While g_runtime_shadow_tick is
// nonzero (only during a healed shard's throwaway validation re-run) runtime_tick
// accumulates the cost into g_runtime_shadow_cycles and pumps nothing (the
// interpreter pass already pumped devices / delivered IRQs). Default 0 → normal
// play and the gcc path are untouched.
extern unsigned          g_runtime_shadow_tick;
extern unsigned long long g_runtime_shadow_cycles;

// Debug PC breakpoint: when nonzero, runtime_should_yield() unwinds the
// current runtime_dispatch the moment the guest PC equals this value.
// Set via the TCP set_break_pc command; 0 = disabled. (MC-HP-002.)
extern uint32_t g_runtime_break_pc;

// ── BIOS / SWI ─────────────────────────────────────────────────────
// SWI emits a call here. The runtime sets up the exception frame
// (LR_svc = PC+4, SPSR_svc = CPSR, mode=SVC, I=1, T=0) and
// dispatches to the recompiled BIOS at 0x00000008 via
// runtime_dispatch. Returns when the recompiled handler issues
// `movs pc, lr` and we land back at the recompiled site. A missed
// BIOS handler self-heals (bridge + on-the-fly recompile + log), it
// is never hand-written HLE — see PRINCIPLES.md "BIOS is sacred —
// recompiled and dispatched" and "Honest self-healing".

void runtime_swi(uint32_t swi_imm);
void runtime_irq(uint32_t return_address);

// ── BIOS HLE hook (opt-in alternative to the recompiled/LLE BIOS) ──────
// nullptr (the default) = pure LLE: runtime_swi always enters SVC mode and
// dispatches the recompiled BIOS, byte-identical to a build without HLE. When
// installed (by gba::bios_hle_set_mode(), see src/runtime/bios_hle.h),
// runtime_swi consults it BEFORE the SVC-mode entry with the decoded SWI number
// (0..0xFF). The hook returns 1 if it fully serviced the call — guest resumes at
// LR, no BIOS dispatch — or 0 to fall through to the recompiled BIOS (the case
// for every SWI the HLE layer does not implement). LLE stays load-bearing and
// remains the verification oracle; HLE never becomes the oracle. This is the
// PRINCIPLES.md "verified-enhancement HLE" carve-out: opt-in, reverts to LLE.
extern int (*g_bios_hle_hook)(uint32_t swi_num);

// ── PSR transfer ───────────────────────────────────────────────────
// MRS/MSR helpers. Routing through the runtime lets us validate mode
// transitions and keep banked registers coherent.
//
// runtime_msr_cpsr handles bank-swap when CPSR.mode changes: the
// outgoing mode's R13/R14 are saved into banked_sp/banked_lr, and
// the incoming mode's are loaded into R[13]/R[14]. FIQ entry/exit
// additionally swaps R8..R12 with r8_12_fiq.

uint32_t runtime_mrs_cpsr(void);
uint32_t runtime_mrs_spsr(void);
void runtime_msr_cpsr(uint32_t value, uint32_t mask);
void runtime_msr_spsr(uint32_t value, uint32_t mask);

// LDM/STM with S=1 and PC absent transfers User-mode registers while
// remaining in the current mode.
uint32_t runtime_read_user_reg(uint32_t reg);
void runtime_write_user_reg(uint32_t reg, uint32_t value);

// ── Exception return ───────────────────────────────────────────────
// Implements the DP-S/LDM-S "Rd=PC" exception return path:
// SPSR_<current mode> → CPSR, bank-swap R13/R14 to the restored
// mode, set PC to new_pc. Used by lowered MOVS PC, LR and LDM with
// PC in list + S bit.

void runtime_exception_return(uint32_t new_pc);
void runtime_restore_cpsr_from_spsr(void);

// ── Codegen gap (NOT a dispatch miss) ──────────────────────────────
// Emitted for IrOps codegen hasn't lowered yet. The runtime ALWAYS
// aborts here — this is a CODEGEN gap, not a dispatch miss, so it is
// NOT self-healed; it is a P0 codegen-completion task. (Analogous to
// the interpreter's own NotImplemented gate, which also stays loud —
// see PRINCIPLES.md "Honest self-healing", "Genuine interpreter gaps
// still abort loudly". Dispatch misses, by contrast, self-heal.)

void runtime_unimplemented_op(const char* op_name, uint32_t pc);

// ── Lifecycle ──────────────────────────────────────────────────────
// Called by the runner before any cart code executes.
// `bus_handle` is a void* pointing to the active gba::GbaBus.

void runtime_init(void* bus_handle);
void runtime_shutdown(void);

#ifdef __cplusplus
}  // extern "C"
#endif
