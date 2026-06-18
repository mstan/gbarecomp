// arm_sljit.cpp — see arm_sljit.h.
//
// FIRST SLICE (P3): the 16 data-processing ops with an IMMEDIATE Op2,
// unconditional (cond AL), no PC operand or destination. This proves the whole
// vertical pipeline — decode -> JIT -> run -> diff vs interpreter INCLUDING
// cycle parity — before widening (shifted operands, flags-carry-in, branches,
// memory, etc. land in P4). Everything outside the slice is DECLINED.
//
// Parity is by construction: the emitter mirrors emit_data_processing in
// arm_codegen.cpp instruction-for-instruction, and reuses the EXACT same flag
// helpers (arm_set_nzc_logic / arm_set_nzcv_add / arm_set_nzcv_sub) the C path
// calls — so the N/Z/C/V results are identical, never re-derived here.

#include "arm_sljit.h"

#include <cstdint>

#include "runtime_arm.h"   // g_cpu, arm_set_*, runtime_tick (addresses baked in)
#include "sljitLir.h"

namespace armv4t {

namespace {

// Byte offsets into ArmCpuState (runtime_arm_types.h: uint32_t R[16]; cpsr; ...)
constexpr sljit_sw kRegOff(unsigned r) { return static_cast<sljit_sw>(r) * 4; }
constexpr sljit_sw kCpsrOff = 16 * 4;

inline sljit_sw addr_of(const void* p) {
    return static_cast<sljit_sw>(reinterpret_cast<uintptr_t>(p));
}

// The set of DP ops slice-1 lowers. ADC/SBC/RSC are excluded (carry-in) until
// P4. TST/TEQ/CMP/CMN write no destination (is_test).
bool is_supported_dp(IrOp op) {
    switch (op) {
        case IrOp::AND: case IrOp::EOR: case IrOp::SUB: case IrOp::RSB:
        case IrOp::ADD: case IrOp::ORR: case IrOp::MOV: case IrOp::BIC:
        case IrOp::MVN: case IrOp::TST: case IrOp::TEQ: case IrOp::CMP:
        case IrOp::CMN:
            return true;
        default:
            return false;
    }
}

bool is_test_op(IrOp op) {
    return op == IrOp::TST || op == IrOp::TEQ ||
           op == IrOp::CMP || op == IrOp::CMN;
}

bool is_logical_op(IrOp op) {
    switch (op) {
        case IrOp::AND: case IrOp::EOR: case IrOp::ORR: case IrOp::BIC:
        case IrOp::MOV: case IrOp::MVN: case IrOp::TST: case IrOp::TEQ:
            return true;
        default:
            return false;
    }
}

bool uses_rn(IrOp op) { return !(op == IrOp::MOV || op == IrOp::MVN); }

}  // namespace

bool sljit_supports(const Instr& ins) {
    if (ins.is_undefined) return false;
    if (ins.cond != Cond::AL) return false;          // conditional → P4
    if (!is_supported_dp(ins.op)) return false;
    if (ins.op2.kind != Op2::Kind::Imm) return false;  // shifted Op2 → P4
    // No PC as a destination (writeback ops only) or as an operand: those need
    // the pc+8 literal / dispatch tail; both land in a later slice.
    if (!is_test_op(ins.op) && ins.rd == 15) return false;
    if (uses_rn(ins.op) && ins.rn == 15) return false;
    return true;
}

SljitFn emit_instr_sljit(const Instr& ins) {
    if (!sljit_supports(ins)) return SljitFn{};

    struct sljit_compiler* C = sljit_create_compiler(nullptr);
    if (!C) return SljitFn{};

    const IrOp op = ins.op;
    const bool test = is_test_op(op);
    const bool logical = is_logical_op(op);
    const uint32_t imm = ins.op2.imm_value;

    // Registers: S0 = &g_cpu, S1 = result, S2 = Rn value. R0..R3 scratch / args.
    sljit_emit_enter(C, 0, SLJIT_ARGS0V(), 4, 3, 0);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S0, 0, SLJIT_IMM, addr_of(&g_cpu));

    // S2 = Rn (ops other than MOV/MVN).
    if (uses_rn(op)) {
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_S2, 0,
                       SLJIT_MEM1(SLJIT_S0), kRegOff(ins.rn));
    }

    // S1 = result (32-bit, matching ARM wraparound).
    switch (op) {
        case IrOp::AND: case IrOp::TST:
            sljit_emit_op2(C, SLJIT_AND32, SLJIT_S1, 0, SLJIT_S2, 0,
                           SLJIT_IMM, imm);
            break;
        case IrOp::EOR: case IrOp::TEQ:
            sljit_emit_op2(C, SLJIT_XOR32, SLJIT_S1, 0, SLJIT_S2, 0,
                           SLJIT_IMM, imm);
            break;
        case IrOp::ORR:
            sljit_emit_op2(C, SLJIT_OR32, SLJIT_S1, 0, SLJIT_S2, 0,
                           SLJIT_IMM, imm);
            break;
        case IrOp::BIC:
            sljit_emit_op2(C, SLJIT_AND32, SLJIT_S1, 0, SLJIT_S2, 0,
                           SLJIT_IMM, static_cast<sljit_sw>(~imm));
            break;
        case IrOp::ADD: case IrOp::CMN:
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S2, 0,
                           SLJIT_IMM, imm);
            break;
        case IrOp::SUB: case IrOp::CMP:
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S2, 0,
                           SLJIT_IMM, imm);
            break;
        case IrOp::RSB:  // imm - Rn
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_IMM, imm,
                           SLJIT_S2, 0);
            break;
        case IrOp::MOV:
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S1, 0, SLJIT_IMM, imm);
            break;
        case IrOp::MVN:
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S1, 0,
                           SLJIT_IMM, static_cast<sljit_sw>(~imm));
            break;
        default:  // unreachable (sljit_supports gate)
            sljit_free_compiler(C);
            return SljitFn{};
    }

    // Writeback (non-test) BEFORE the flag call, which clobbers scratch regs.
    if (!test) {
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(ins.rd),
                       SLJIT_S1, 0);
    }

    // Flags. Reuse the exact C-path helpers (parity by construction).
    if (ins.set_flags) {
        if (logical) {
            // N/Z from result; C from the shifter carry-out. For imm Op2 the
            // decoder pre-computes imm_carry_out: 0/1, or 2 = "carry unchanged"
            // (== current cpsr_c()).
            if (ins.op2.imm_carry_out == 2u) {
                sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_S0), kCpsrOff);
                sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_R0, 0, SLJIT_R0, 0,
                               SLJIT_IMM, 29);
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0,
                               SLJIT_IMM, 1);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_R0, 0);  // carry
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);  // result
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);  // result
                sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
                               static_cast<sljit_sw>(ins.op2.imm_carry_out & 1u));
            }
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(32, 32),
                             SLJIT_IMM, addr_of((const void*)&arm_set_nzc_logic));
        } else {
            // Arithmetic: arm_set_nzcv_add/sub(a, b, result). ADD/CMN add;
            // SUB/CMP/RSB sub (RSB swaps operands: a=imm, b=Rn).
            const bool add = (op == IrOp::ADD || op == IrOp::CMN);
            if (op == IrOp::RSB) {
                sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, imm);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S2, 0);
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S2, 0);
                sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, imm);
            }
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_S1, 0);
            const void* helper = add ? (const void*)&arm_set_nzcv_add
                                     : (const void*)&arm_set_nzcv_sub;
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(32, 32, 32),
                             SLJIT_IMM, addr_of(helper));
        }
    }

    // Cycle tick: DP cost is a fixed 1S (instr_cycle_base==1; no register-shift
    // surcharge in slice-1, no PC write). One runtime_tick at the boundary,
    // exactly as the C epilogue does.
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 1);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32),
                     SLJIT_IMM, addr_of((const void*)&runtime_tick));

    sljit_emit_return_void(C);

    void* code = sljit_generate_code(C, 0, nullptr);
    sljit_uw size = sljit_get_generated_code_size(C);
    sljit_free_compiler(C);
    if (!code) return SljitFn{};

    SljitFn out;
    out.fn = reinterpret_cast<void (*)(void)>(code);
    out.code = code;
    out.code_size = static_cast<unsigned long>(size);
    return out;
}

void free_sljit_fn(const SljitFn& f) {
    if (f.code) sljit_free_code(f.code, nullptr);
}

}  // namespace armv4t
