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
// Banked registers + mode switching live in the C++ runtime; rare
// events route through runtime_msr / runtime_mode_switch.

typedef struct ArmCpuState {
    uint32_t R[16];
    uint32_t cpsr;
} ArmCpuState;

extern ArmCpuState g_cpu;

// Convenience accessors. CSR-bit constants follow ARM ARM A2.5.
#define CPSR_N_BIT (1u << 31)
#define CPSR_Z_BIT (1u << 30)
#define CPSR_C_BIT (1u << 29)
#define CPSR_V_BIT (1u << 28)
#define CPSR_I_BIT (1u <<  7)
#define CPSR_F_BIT (1u <<  6)
#define CPSR_T_BIT (1u <<  5)

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

// ── BIOS / SWI ─────────────────────────────────────────────────────
// SWI emits a call here. The runtime sets up the exception frame
// (LR_svc = PC+4, SPSR_svc = CPSR, mode=SVC, I=1, T=0) and
// dispatches to the recompiled BIOS at 0x00000008 via
// runtime_dispatch. Returns when the recompiled handler issues
// `movs pc, lr` and we land back at the recompiled site. There is
// no interpreter fallback — see PRINCIPLES.md "Interpreter is
// informative, never load-bearing (SHOWSTOPPER)".

void runtime_swi(uint32_t swi_imm);

// ── PSR transfer ───────────────────────────────────────────────────
// MRS/MSR helpers. Routing through the runtime lets us validate mode
// transitions and keep banked registers coherent.

uint32_t runtime_mrs_cpsr(void);
uint32_t runtime_mrs_spsr(void);
void runtime_msr_cpsr(uint32_t value, uint32_t mask);
void runtime_msr_spsr(uint32_t value, uint32_t mask);

// ── Fallback ───────────────────────────────────────────────────────
// Emitted for IrOps codegen hasn't lowered yet. The runtime ALWAYS
// aborts here — there is no interpreter fallback (see PRINCIPLES.md
// "Interpreter is informative, never load-bearing"). Every abort is
// a P0 codegen-completion task.

void runtime_unimplemented_op(const char* op_name, uint32_t pc);

// ── Lifecycle ──────────────────────────────────────────────────────
// Called by the runner before any cart code executes.
// `bus_handle` is a void* pointing to the active gba::GbaBus.

void runtime_init(void* bus_handle);
void runtime_shutdown(void);

#ifdef __cplusplus
}  // extern "C"
#endif
