// runtime_arm.cpp — implementation of the C-ABI surface that the
// recompiled cart code calls into.
//
// The interpreter encodes the canonical ARM/THUMB semantics; this
// file is the parallel surface for recompiled code. Helpers here
// must produce the SAME bit-level result as the interpreter would
// for the same inputs — verify any new helper against the
// interpreter's case in src/armv4t/interpreter.cpp.

#include "runtime_arm.h"

#include <cstdio>
#include <cstdlib>

// Forward decls of the per-game dispatch table. Each game's
// generated/dispatch_table.cpp defines these.
struct DispatchEntry {
    uint32_t addr;
    void (*fn)(void);
};
extern "C" const DispatchEntry kDispatchTable[];
extern "C" const unsigned kDispatchTableLen;

// ── CPU state ──────────────────────────────────────────────────────

extern "C" ArmCpuState g_cpu = {};

// ── Bus binding ────────────────────────────────────────────────────

namespace gbarecomp {
namespace runtime_arm {

// We keep this as a void* so the header doesn't need to drag in
// GbaBus. The bus type is known to the implementation file only.
void* g_bus_handle = nullptr;

}  // namespace runtime_arm
}  // namespace gbarecomp

// ── Condition codes ────────────────────────────────────────────────

extern "C" int arm_cond_passes(unsigned cond) {
    // The condition codes are 4 bits. AL / NV are the unconditional
    // bands.
    const uint32_t n = cpsr_n();
    const uint32_t z = cpsr_z();
    const uint32_t c = cpsr_c();
    const uint32_t v = cpsr_v();
    switch (cond & 0xFu) {
        case 0x0: return z != 0;                            // EQ
        case 0x1: return z == 0;                            // NE
        case 0x2: return c != 0;                            // CS/HS
        case 0x3: return c == 0;                            // CC/LO
        case 0x4: return n != 0;                            // MI
        case 0x5: return n == 0;                            // PL
        case 0x6: return v != 0;                            // VS
        case 0x7: return v == 0;                            // VC
        case 0x8: return (c != 0) && (z == 0);              // HI
        case 0x9: return (c == 0) || (z != 0);              // LS
        case 0xA: return n == v;                            // GE
        case 0xB: return n != v;                            // LT
        case 0xC: return (z == 0) && (n == v);              // GT
        case 0xD: return (z != 0) || (n != v);              // LE
        case 0xE: return 1;                                 // AL
        case 0xF: return 0;                                 // NV (ARMv4T: never)
        default:  return 0;
    }
}

// ── Bus accessors ──────────────────────────────────────────────────
//
// The runtime initializer (runtime_init) installs `g_bus_handle` to
// a pointer to the active gba::GbaBus. Calling the bus is the only
// per-instruction host-side work the generated code has to do, so
// these need to be fast.
//
// For first cut we delegate via the bus_handle as a fat pointer. A
// later optimization can compile bus reads inline using the
// runtime's memory map directly.

namespace {

inline uint32_t* bus_ptr32(void* /*h*/, uint32_t /*addr*/) {
    return nullptr;  // not used; placeholder for future inlining
}

}  // namespace

// Bus accessors are NOT defined here — they live in
// src/runtime/runtime_bus_bridge.cpp (part of gbarecomp_runtime),
// which is the only translation unit allowed to include gba_bus.h.
// Anything that links against gbarecomp_runtime gets them; tests
// that link only gbarecomp_armv4t can supply their own stubs.

// ── Shifter helpers ────────────────────────────────────────────────

extern "C" uint32_t arm_shift_lsl(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;  // ARM ARM A5.1.5: shift by 0 → no carry update
    if (n >= 32) {
        if (set_carry) {
            uint32_t carry = (n == 32) ? (v & 1u) : 0u;
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return 0;
    }
    if (set_carry) {
        uint32_t carry = (v >> (32u - n)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
    }
    return v << n;
}

extern "C" uint32_t arm_shift_lsr(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;
    if (n >= 32) {
        if (set_carry) {
            uint32_t carry = (n == 32) ? ((v >> 31) & 1u) : 0u;
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return 0;
    }
    if (set_carry) {
        uint32_t carry = (v >> (n - 1u)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
    }
    return v >> n;
}

extern "C" uint32_t arm_shift_asr(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;
    if (n >= 32) {
        uint32_t carry = (v >> 31) & 1u;
        if (set_carry) {
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return carry ? 0xFFFFFFFFu : 0u;
    }
    if (set_carry) {
        uint32_t carry = (v >> (n - 1u)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
    }
    return static_cast<uint32_t>(static_cast<int32_t>(v) >> n);
}

extern "C" uint32_t arm_shift_ror(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;
    n &= 31u;
    if (n == 0) {  // ROR by multiple of 32 → no change but use top bit as carry
        if (set_carry) {
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) |
                ((v & 0x80000000u) ? CPSR_C_BIT : 0u);
        }
        return v;
    }
    uint32_t r = (v >> n) | (v << (32u - n));
    if (set_carry) {
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) |
            ((r & 0x80000000u) ? CPSR_C_BIT : 0u);
    }
    return r;
}

// ── Flag updaters ──────────────────────────────────────────────────

extern "C" void arm_set_nz(uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    g_cpu.cpsr = c;
}

extern "C" void arm_set_nzc_logic(uint32_t r, uint32_t shifter_carry) {
    // shifter_carry is the carry-out from the operand2 shifter, or
    // the existing CPSR.C if the shift didn't produce one (encoded
    // by the codegen as `cpsr_c()`).
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    if (shifter_carry)   c |= CPSR_C_BIT;
    g_cpu.cpsr = c;
}

extern "C" void arm_set_nzcv_add(uint32_t a, uint32_t b, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                  CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    // Carry: unsigned overflow.
    if (r < a)           c |= CPSR_C_BIT;
    // Overflow: same-sign inputs, different-sign result.
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

extern "C" void arm_set_nzcv_adc(uint32_t a, uint32_t b, uint32_t c_in,
                                   uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                  CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    // Carry from full 33-bit addition.
    uint64_t wide = static_cast<uint64_t>(a) + b + c_in;
    if (wide >> 32)      c |= CPSR_C_BIT;
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

extern "C" void arm_set_nzcv_sub(uint32_t a, uint32_t b, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                  CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    // Carry: NOT borrow. C=1 if a >= b.
    if (a >= b)          c |= CPSR_C_BIT;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

extern "C" void arm_set_nzcv_sbc(uint32_t a, uint32_t b, uint32_t c_in,
                                   uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                  CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    // SBC: r = a - b - (1 - c_in). Carry-out = NOT borrow.
    // The 33-bit equivalent: a + ~b + c_in. Overflow if top bit.
    uint64_t wide = static_cast<uint64_t>(a) + (~b & 0xFFFFFFFFu) + c_in;
    if (wide >> 32)      c |= CPSR_C_BIT;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

// ── Dispatch ───────────────────────────────────────────────────────

namespace {

// Binary search the dispatch table for `pc`. Returns the entry
// index, or kDispatchTableLen if not found.
unsigned dispatch_lookup(uint32_t pc) {
    unsigned lo = 0;
    unsigned hi = kDispatchTableLen;
    while (lo < hi) {
        unsigned mid = (lo + hi) >> 1u;
        if (kDispatchTable[mid].addr < pc) lo = mid + 1u;
        else                                hi = mid;
    }
    if (lo < kDispatchTableLen && kDispatchTable[lo].addr == pc) {
        return lo;
    }
    return kDispatchTableLen;
}

}  // namespace

extern "C" void runtime_dispatch(uint32_t target_pc) {
    // Strip THUMB bit; codegen handles the mode via cpsr_T already.
    uint32_t pc = target_pc & ~1u;
    unsigned idx = dispatch_lookup(pc);
    if (idx < kDispatchTableLen) {
        kDispatchTable[idx].fn();
        return;
    }
    runtime_dispatch_miss(target_pc);
}

extern "C" void runtime_dispatch_with_exchange(uint32_t target_pc) {
    // Bit 0 of target indicates THUMB.
    if (target_pc & 1u) g_cpu.cpsr |= CPSR_T_BIT;
    else                g_cpu.cpsr &= ~CPSR_T_BIT;
    runtime_dispatch(target_pc);
}

extern "C" void runtime_dispatch_miss(uint32_t target_pc) {
    std::fprintf(stderr,
                 "runtime_arm: dispatch miss for pc=0x%08X "
                 "(no generated function; not recompiled, "
                 "or function-finder didn't reach it). "
                 "Add to game.toml [functions] and regen.\n",
                 target_pc);
    std::abort();
}

// ── BIOS / SWI ─────────────────────────────────────────────────────
// Dispatches to the recompiled BIOS at 0x00000008. NO interpreter
// fallback (PRINCIPLES.md "Interpreter is informative, never
// load-bearing"). Until the BIOS dispatch table exists this aborts
// — that abort is a P0 BIOS-recompilation gate.

extern "C" void runtime_swi(uint32_t swi_imm) {
    std::fprintf(stderr,
                 "runtime_arm: runtime_swi(0x%X) called — BIOS "
                 "dispatch table not yet populated. Recompile the "
                 "BIOS and register entries; no interpreter "
                 "fallback is permitted.\n",
                 swi_imm);
    std::abort();
}

// ── PSR transfer ───────────────────────────────────────────────────

extern "C" uint32_t runtime_mrs_cpsr(void) {
    return g_cpu.cpsr;
}

extern "C" uint32_t runtime_mrs_spsr(void) {
    // SPSR_<mode> lives in the C++ runtime's banked storage.
    // Trampoline added in a sibling translation unit.
    std::fprintf(stderr,
                 "runtime_arm: runtime_mrs_spsr unbound\n");
    return 0;
}

extern "C" void runtime_msr_cpsr(uint32_t value, uint32_t mask) {
    // mask is the 4-bit field-mask from the encoding; expand to
    // a byte-wise bitmask over CPSR.
    uint32_t bytewise = 0;
    if (mask & 1u) bytewise |= 0x000000FFu;   // control byte
    if (mask & 2u) bytewise |= 0x0000FF00u;   // ext1 (reserved)
    if (mask & 4u) bytewise |= 0x00FF0000u;   // ext2 (reserved)
    if (mask & 8u) bytewise |= 0xFF000000u;   // flags
    g_cpu.cpsr = (g_cpu.cpsr & ~bytewise) | (value & bytewise);
}

extern "C" void runtime_msr_spsr(uint32_t value,
                                                         uint32_t mask) {
    (void)value; (void)mask;
    std::fprintf(stderr,
                 "runtime_arm: runtime_msr_spsr unbound\n");
}

// ── Fallback ───────────────────────────────────────────────────────

extern "C" void runtime_unimplemented_op(
    const char* op_name, uint32_t pc) {
    std::fprintf(stderr,
                 "runtime_arm: unimplemented op %s at pc=0x%08X\n",
                 op_name ? op_name : "(null)", pc);
    std::abort();
}

// ── Lifecycle ──────────────────────────────────────────────────────

extern "C" void runtime_init(void* bus_handle) {
    gbarecomp::runtime_arm::g_bus_handle = bus_handle;
}

extern "C" void runtime_shutdown(void) {
    gbarecomp::runtime_arm::g_bus_handle = nullptr;
}
