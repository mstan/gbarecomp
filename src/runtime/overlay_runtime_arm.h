// overlay_runtime_arm.h — the Stage-2 overlay shim.
//
// A runtime-recompiled overlay .c includes THIS instead of "runtime_arm.h".
// The generated function body is byte-identical to the static corpus — it
// still says `g_cpu.R[15] = ...`, `runtime_tick(...)`, `bus_read_u32(...)`,
// etc. The only difference is the prelude: here, `g_cpu` is a macro for
// `(*g_ovl->cpu)` and every runtime_*/bus_*/arm_* name is a static-inline
// thunk that forwards through the GbaOverlayCallbacks the host installed at
// overlay_init(). So the DLL has ZERO unresolved host symbols, yet mutates
// the host's real CPU state through the pointer.
//
// Identical body + identical semantics is exactly what makes a healed run's
// per-instruction fingerprint ring byte-identical to the static build.

#pragma once

#include <stdint.h>

#include "overlay_abi.h"  // GbaOverlayCallbacks, ArmCpuState, CPSR_*_BIT

// Installed once per DLL by overlay_init() (the overlay .c defines it).
extern const GbaOverlayCallbacks* g_ovl;

// The host register file, reached through the callbacks pointer. Generated
// code reads/writes g_cpu.R[...] / g_cpu.cpsr exactly as in the static build.
#define g_cpu (*g_ovl->cpu)
#define g_runtime_insn_trace (*g_ovl->runtime_insn_trace)
#define g_runtime_cycles (*g_ovl->runtime_cycles)
#define g_runtime_break_pc (*g_ovl->runtime_break_pc)

// CPSR flag accessors (generated code uses cpsr_c for ADC/SBC carry-in).
static inline uint32_t cpsr_n(void) { return (g_ovl->cpu->cpsr & CPSR_N_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_z(void) { return (g_ovl->cpu->cpsr & CPSR_Z_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_c(void) { return (g_ovl->cpu->cpsr & CPSR_C_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_v(void) { return (g_ovl->cpu->cpsr & CPSR_V_BIT) ? 1u : 0u; }

// ── Bus ──
static inline uint32_t bus_read_u32(uint32_t a) { return g_ovl->bus_read_u32(a); }
static inline uint16_t bus_read_u16(uint32_t a) { return g_ovl->bus_read_u16(a); }
static inline uint8_t  bus_read_u8 (uint32_t a) { return g_ovl->bus_read_u8(a); }
static inline void bus_write_u32(uint32_t a, uint32_t v) { g_ovl->bus_write_u32(a, v); }
static inline void bus_write_u16(uint32_t a, uint16_t v) { g_ovl->bus_write_u16(a, v); }
static inline void bus_write_u8 (uint32_t a, uint8_t  v) { g_ovl->bus_write_u8(a, v); }

// ── Condition / shifter / flags ──
static inline int arm_cond_passes(unsigned c) { return g_ovl->arm_cond_passes(c); }
static inline uint32_t arm_shift_lsl(uint32_t v, uint32_t n, int sc) { return g_ovl->arm_shift_lsl(v, n, sc); }
static inline uint32_t arm_shift_lsr(uint32_t v, uint32_t n, int sc) { return g_ovl->arm_shift_lsr(v, n, sc); }
static inline uint32_t arm_shift_asr(uint32_t v, uint32_t n, int sc) { return g_ovl->arm_shift_asr(v, n, sc); }
static inline uint32_t arm_shift_ror(uint32_t v, uint32_t n, int sc) { return g_ovl->arm_shift_ror(v, n, sc); }
static inline void arm_set_nz(uint32_t r) { g_ovl->arm_set_nz(r); }
static inline void arm_set_nzc_logic(uint32_t r, uint32_t sc) { g_ovl->arm_set_nzc_logic(r, sc); }
static inline void arm_set_nzcv_add(uint32_t a, uint32_t b, uint32_t r) { g_ovl->arm_set_nzcv_add(a, b, r); }
static inline void arm_set_nzcv_adc(uint32_t a, uint32_t b, uint32_t ci, uint32_t r) { g_ovl->arm_set_nzcv_adc(a, b, ci, r); }
static inline void arm_set_nzcv_sub(uint32_t a, uint32_t b, uint32_t r) { g_ovl->arm_set_nzcv_sub(a, b, r); }
static inline void arm_set_nzcv_sbc(uint32_t a, uint32_t b, uint32_t ci, uint32_t r) { g_ovl->arm_set_nzcv_sbc(a, b, ci, r); }

// ── Dispatch / call-return ──
static inline void runtime_dispatch(uint32_t pc) { g_ovl->runtime_dispatch(pc); }
static inline void runtime_dispatch_with_exchange(uint32_t pc) { g_ovl->runtime_dispatch_with_exchange(pc); }
static inline void runtime_call_push_return(uint32_t pc) { g_ovl->runtime_call_push_return(pc); }
static inline int  runtime_call_should_return(uint32_t pc) { return g_ovl->runtime_call_should_return(pc); }
static inline void runtime_call_cancel_return(uint32_t pc) { g_ovl->runtime_call_cancel_return(pc); }

// ── Timing / scheduling ──
static inline void runtime_tick(uint32_t c) { g_ovl->runtime_tick(c); }
static inline int  runtime_should_yield(void) { return g_ovl->runtime_should_yield(); }
static inline uint32_t runtime_mem_cycles(uint32_t a, uint32_t w, uint32_t s) { return g_ovl->runtime_mem_cycles(a, w, s); }
static inline uint32_t runtime_mul_cycles(uint32_t rs, uint32_t sv, uint32_t ex) { return g_ovl->runtime_mul_cycles(rs, sv, ex); }

// ── Exceptions / PSR / mode ──
static inline void runtime_swi(uint32_t imm) { g_ovl->runtime_swi(imm); }
static inline void runtime_irq(uint32_t ret) { g_ovl->runtime_irq(ret); }
static inline uint32_t runtime_mrs_cpsr(void) { return g_ovl->runtime_mrs_cpsr(); }
static inline uint32_t runtime_mrs_spsr(void) { return g_ovl->runtime_mrs_spsr(); }
static inline void runtime_msr_cpsr(uint32_t v, uint32_t m) { g_ovl->runtime_msr_cpsr(v, m); }
static inline void runtime_msr_spsr(uint32_t v, uint32_t m) { g_ovl->runtime_msr_spsr(v, m); }
static inline uint32_t runtime_read_user_reg(uint32_t r) { return g_ovl->runtime_read_user_reg(r); }
static inline void runtime_write_user_reg(uint32_t r, uint32_t v) { g_ovl->runtime_write_user_reg(r, v); }
static inline void runtime_exception_return(uint32_t pc) { g_ovl->runtime_exception_return(pc); }
static inline void runtime_restore_cpsr_from_spsr(void) { g_ovl->runtime_restore_cpsr_from_spsr(); }

// ── Instrumentation ──
static inline void runtime_insn_fp(void) { g_ovl->runtime_insn_fp(); }
static inline void runtime_trace_event(uint32_t k, uint32_t pc, uint32_t a, uint32_t v, uint32_t aux) { g_ovl->runtime_trace_event(k, pc, a, v, aux); }
static inline void runtime_unimplemented_op(const char* op, uint32_t pc) { g_ovl->runtime_unimplemented_op(op, pc); }
