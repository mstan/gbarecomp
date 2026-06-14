// runtime_arm_types.h — POD types + bit/constant macros shared by the
// recompiler ABI surface (runtime_arm.h) AND the Stage-2 self-healing
// overlay shim (src/runtime/overlay_runtime_arm.h, overlay_abi.h).
//
// This header carries ONLY plain types and #defines — NO `extern g_cpu`,
// NO function declarations, NO inline functions. That separation is what
// lets the overlay shim `#define g_cpu (*g_ovl->cpu)` and provide its own
// thunks without colliding with runtime_arm.h's real declarations.
//
// ArmCpuState's layout is a hard cross-module contract: a runtime-compiled
// overlay DLL reads/writes the host exe's `g_cpu` through a pointer in the
// callbacks struct, so both sides MUST agree on this struct byte-for-byte.
// Keeping it here (single source of truth) is deliberate — never duplicate it.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bank indexes match armv4t::BankedSlot — User=0, FIQ=1, IRQ=2,
// Supervisor=3, Abort=4, Undefined=5.
#define ARM_BANK_USER       0u
#define ARM_BANK_FIQ        1u
#define ARM_BANK_IRQ        2u
#define ARM_BANK_SUPERVISOR 3u
#define ARM_BANK_ABORT      4u
#define ARM_BANK_UNDEFINED  5u
#define ARM_BANK_COUNT      6u

typedef struct ArmCpuState {
    uint32_t R[16];
    uint32_t cpsr;

    // Banked storage. SPSR for User/System is undefined and unused.
    uint32_t banked_sp[ARM_BANK_COUNT];
    uint32_t banked_lr[ARM_BANK_COUNT];
    uint32_t banked_spsr[ARM_BANK_COUNT];

    // R8..R12 have a parallel bank for FIQ. The active values live
    // in R[8..12]; the inactive bank is mirrored here.
    uint32_t r8_12_user[5];
    uint32_t r8_12_fiq[5];
} ArmCpuState;

// CSR-bit constants follow ARM ARM A2.5.
#define CPSR_N_BIT (1u << 31)
#define CPSR_Z_BIT (1u << 30)
#define CPSR_C_BIT (1u << 29)
#define CPSR_V_BIT (1u << 28)
#define CPSR_I_BIT (1u <<  7)
#define CPSR_F_BIT (1u <<  6)
#define CPSR_T_BIT (1u <<  5)

// Always-on structured execution trace kinds.
#define RUNTIME_TRACE_DISPATCH  1u
#define RUNTIME_TRACE_EXCHANGE  2u
#define RUNTIME_TRACE_SWI       3u
#define RUNTIME_TRACE_MEM_WRITE 4u
#define RUNTIME_TRACE_BRANCH    5u
#define RUNTIME_TRACE_IRQ       6u
// RUNTIME_TRACE_CALL aux values:
//   1 push, 2 top-frame return, 3 no match, 4 cancel, 5 non-local return.
#define RUNTIME_TRACE_CALL      7u
#define RUNTIME_TRACE_MEM_READ  8u

typedef struct RuntimeTraceEntry {
    uint32_t seq;
    uint64_t cycles;  // cumulative guest cycles at the moment of this event
    uint32_t kind;
    uint32_t pc;
    uint32_t cpsr;
    uint32_t addr;
    uint32_t value;
    uint32_t aux;
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r4;
    uint32_t r5;
    uint32_t r12;
    uint32_t r13;
    uint32_t r14;
} RuntimeTraceEntry;

typedef struct RuntimeFpEntry {
    unsigned long long cycles;  // cumulative guest cycles BEFORE this instruction
    uint32_t pc;
    uint32_t cpsr;
    uint32_t r[16];
} RuntimeFpEntry;

#ifdef __cplusplus
}  // extern "C"
#endif
