// overlay_abi.h — the ONLY contract between the host exe and a
// runtime-compiled overlay DLL (Stage-2 self-healing recompiler).
//
// A dispatch miss → emit that one function's C → gcc → DLL → LoadLibrary.
// The DLL has ZERO unresolved host symbols: at load it is handed a
// GbaOverlayCallbacks* via the exported overlay_init(), holding a pointer
// to the host's g_cpu and one function pointer per runtime/bus/arm ABI
// symbol the generated code may call. This is the psxrecomp model — it
// avoids exporting g_cpu (a DATA symbol) from the exe, whose MinGW data
// auto-import is unreliable and would be catastrophic if silently wrong.
//
// VERSIONING: GBA_OVERLAY_ABI_VERSION gates every load. Bump it on ANY
// change to this struct OR to ArmCpuState's layout (runtime_arm_types.h) —
// a cached DLL built against a different ABI is rejected (FreeLibrary +
// delete + recompile). The struct is APPEND-ONLY: never reorder or remove.

#pragma once

#include <stdint.h>

#include "runtime_arm_types.h"  // ArmCpuState (shared layout)

#ifdef __cplusplus
extern "C" {
#endif

#define GBA_OVERLAY_ABI_VERSION 3u

typedef struct GbaOverlayCallbacks {
    uint32_t abi_version;  // must equal GBA_OVERLAY_ABI_VERSION

    // ── Host global state (shared by pointer, never copied) ──
    ArmCpuState*        cpu;                 // &g_cpu
    unsigned*           runtime_insn_trace;  // &g_runtime_insn_trace
    unsigned long long* runtime_cycles;      // &g_runtime_cycles
    uint32_t*           runtime_break_pc;    // &g_runtime_break_pc

    // ── Bus ──
    uint32_t (*bus_read_u32)(uint32_t addr);
    uint16_t (*bus_read_u16)(uint32_t addr);
    uint8_t  (*bus_read_u8)(uint32_t addr);
    void     (*bus_write_u32)(uint32_t addr, uint32_t val);
    void     (*bus_write_u16)(uint32_t addr, uint16_t val);
    void     (*bus_write_u8)(uint32_t addr, uint8_t val);

    // ── Condition / shifter / flags ──
    int      (*arm_cond_passes)(unsigned cond);
    uint32_t (*arm_shift_lsl)(uint32_t v, uint32_t n, int set_carry);
    uint32_t (*arm_shift_lsr)(uint32_t v, uint32_t n, int set_carry);
    uint32_t (*arm_shift_asr)(uint32_t v, uint32_t n, int set_carry);
    uint32_t (*arm_shift_ror)(uint32_t v, uint32_t n, int set_carry);
    void     (*arm_set_nz)(uint32_t result);
    void     (*arm_set_nzc_logic)(uint32_t result, uint32_t shifter_carry);
    void     (*arm_set_nzcv_add)(uint32_t a, uint32_t b, uint32_t result);
    void     (*arm_set_nzcv_adc)(uint32_t a, uint32_t b, uint32_t c_in, uint32_t result);
    void     (*arm_set_nzcv_sub)(uint32_t a, uint32_t b, uint32_t result);
    void     (*arm_set_nzcv_sbc)(uint32_t a, uint32_t b, uint32_t c_in, uint32_t result);

    // ── Dispatch / call-return ──
    void     (*runtime_dispatch)(uint32_t target_pc);
    void     (*runtime_dispatch_with_exchange)(uint32_t target_pc);
    void     (*runtime_call_push_return)(uint32_t return_pc);
    int      (*runtime_call_should_return)(uint32_t target_pc);
    void     (*runtime_call_cancel_return)(uint32_t return_pc);

    // ── Timing / scheduling ──
    void     (*runtime_tick)(uint32_t cycles);
    int      (*runtime_should_yield)(void);   // bool in C++; int is layout-compatible
    uint32_t (*runtime_mem_cycles)(uint32_t addr, uint32_t width, uint32_t sequential);
    uint32_t (*runtime_mul_cycles)(uint32_t rs_value, uint32_t signed_variant, uint32_t extra);

    // ── Exceptions / PSR / mode ──
    void     (*runtime_swi)(uint32_t swi_imm);
    void     (*runtime_irq)(uint32_t return_address);
    uint32_t (*runtime_mrs_cpsr)(void);
    uint32_t (*runtime_mrs_spsr)(void);
    void     (*runtime_msr_cpsr)(uint32_t value, uint32_t mask);
    void     (*runtime_msr_spsr)(uint32_t value, uint32_t mask);
    uint32_t (*runtime_read_user_reg)(uint32_t reg);
    void     (*runtime_write_user_reg)(uint32_t reg, uint32_t value);
    void     (*runtime_exception_return)(uint32_t new_pc);
    void     (*runtime_restore_cpsr_from_spsr)(void);

    // ── Instrumentation ──
    void     (*runtime_insn_fp)(void);
    void     (*runtime_trace_event)(uint32_t kind, uint32_t pc, uint32_t addr,
                                    uint32_t value, uint32_t aux);
    void     (*runtime_unimplemented_op)(const char* op_name, uint32_t pc);

    // APPEND new members BELOW this line only; bump GBA_OVERLAY_ABI_VERSION.

    // ── ABI v2 ── the recompiler emits a function-entry hook call into every
    // generated prologue (emit_function.cpp); the overlay body references the
    // host's g_runtime_fn_entry_hook through this pointer (NULL hook = skipped).
    void (**runtime_fn_entry_hook)(uint32_t entry_pc);  // &g_runtime_fn_entry_hook
    void (*runtime_idle_backedge)(uint32_t header_pc);
} GbaOverlayCallbacks;

// Exported by every overlay DLL:
//   uint32_t overlay_abi(void);                       — returns the ABI version
//   void     overlay_init(const GbaOverlayCallbacks*); — stores the callbacks
//   void     func_XXXXXXXX(void);                      — the recompiled function

#ifdef __cplusplus
}  // extern "C"
#endif
