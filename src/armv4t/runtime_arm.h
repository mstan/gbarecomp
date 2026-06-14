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
uint32_t runtime_trace_copy_recent(RuntimeTraceEntry* out,
                                   uint32_t max_entries);
void runtime_tick(uint32_t cycles);
bool runtime_should_yield(void);
// Cumulative guest-cycle clock. Incremented by runtime_tick on EVERY tick
// (per-instruction exec ticks AND halt-pump chunks), so it is the authoritative
// total-cycle count — unlike runtime.cpp's `cycles_elapsed`, which only tallied
// the halt path (the MC-HP-002 "cycles incomparable" red herring). Reset to 0
// at machine reset (runtime_trace_reset). Stamped onto every ring entry so the
// recomp and interp oracle can be aligned by identical cycle counts. (MC-HP-002.)
extern unsigned long long g_runtime_cycles;
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
