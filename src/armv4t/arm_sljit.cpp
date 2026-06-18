// arm_sljit.cpp — see arm_sljit.h.
//
// COVERAGE (grows under the L1 JIT harness, one class at a time):
//   slice 1 (P3): 16 data-processing ops, IMMEDIATE Op2, cond AL, no PC.
//   P4 step 1:    + SHIFTED Op2 with an IMMEDIATE shift count (LSL/LSR/ASR/
//                   ROR/RRX), incl. the shifter carry-out for S=1 logical ops.
// Still DECLINED (precision over recall → caller runs gcc / interpreter):
//   register-specified shift counts (by_register), R15 as operand/dest,
//   conditional execution, ADC/SBC/RSC (carry-in), branches, memory, LDM/STM,
//   multiplies, PSR, SWI. Each lands in a later step, gated by the harness.
//
// Parity is by construction: the value/carry math mirrors emit_op2 in
// arm_codegen.cpp, and flags reuse the EXACT C-path helpers (arm_set_nzc_logic /
// arm_set_nzcv_add / arm_set_nzcv_sub) — never re-derived here.

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

bool is_supported_dp(IrOp op) {
    switch (op) {
        case IrOp::AND: case IrOp::EOR: case IrOp::SUB: case IrOp::RSB:
        case IrOp::ADD: case IrOp::ADC: case IrOp::SBC: case IrOp::RSC:
        case IrOp::ORR: case IrOp::MOV: case IrOp::BIC:
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
    if (ins.cond != Cond::AL) return false;          // conditional → later
    if (!is_supported_dp(ins.op)) return false;
    if (ins.op2.kind == Op2::Kind::Shifted) {
        if (ins.op2.shifted.by_register) return false;  // register count → later
        if (ins.op2.shifted.rm == 15) return false;     // PC operand → later
    }
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
    const bool need_carry = ins.set_flags && logical;  // shifter carry feeds C

    // S0 = &g_cpu, S1 = result, S2 = Rn, S3 = Op2 value, S4 = Op2 carry-out.
    sljit_emit_enter(C, 0, SLJIT_ARGS0V(), 4, 5, 0);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S0, 0, SLJIT_IMM, addr_of(&g_cpu));

    // cpsr_c() -> dst (the unchanged-carry source). Bit 29 of cpsr.
    auto emit_cpsr_c = [&](sljit_s32 dst) {
        sljit_emit_op1(C, SLJIT_MOV_U32, dst, 0, SLJIT_MEM1(SLJIT_S0), kCpsrOff);
        sljit_emit_op2(C, SLJIT_LSHR32, dst, 0, dst, 0, SLJIT_IMM, 29);
        sljit_emit_op2(C, SLJIT_AND32, dst, 0, dst, 0, SLJIT_IMM, 1);
    };

    // ── Op2 value -> S3 (and carry -> S4 when need_carry) ──
    if (ins.op2.kind == Op2::Kind::Imm) {
        const uint32_t imm = ins.op2.imm_value;
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S3, 0, SLJIT_IMM, imm);
        if (need_carry) {
            if (ins.op2.imm_carry_out == 2u) {
                emit_cpsr_c(SLJIT_S4);
            } else {
                sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S4, 0, SLJIT_IMM,
                               static_cast<sljit_sw>(ins.op2.imm_carry_out & 1u));
            }
        }
    } else {
        // Shifted register, immediate count. R0 = Rm. Mirrors emit_op2's
        // imm-count branch, value -> S3, carry -> S4.
        const auto& sr = ins.op2.shifted;
        const unsigned n = sr.imm_or_rs;
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_S0), kRegOff(sr.rm));
        const bool is_rrx =
            (sr.type == ShiftType::RRX) ||
            (sr.type == ShiftType::ROR && n == 0);

        if (is_rrx) {
            // val = (Rm >> 1) | (cpsr_c() << 31); carry = Rm & 1.
            if (need_carry)
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_S4, 0, SLJIT_R0, 0,
                               SLJIT_IMM, 1);
            emit_cpsr_c(SLJIT_R1);
            sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R1, 0, SLJIT_R1, 0,
                           SLJIT_IMM, 31);
            sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S3, 0, SLJIT_R0, 0,
                           SLJIT_IMM, 1);
            sljit_emit_op2(C, SLJIT_OR32, SLJIT_S3, 0, SLJIT_S3, 0, SLJIT_R1, 0);
        } else switch (sr.type) {
            case ShiftType::LSL:
                if (n == 0) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S3, 0, SLJIT_R0, 0);
                    if (need_carry) emit_cpsr_c(SLJIT_S4);
                } else {
                    if (need_carry) {
                        sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_R0, 0,
                                       SLJIT_IMM, 32u - n);
                        sljit_emit_op2(C, SLJIT_AND32, SLJIT_S4, 0, SLJIT_S4, 0,
                                       SLJIT_IMM, 1);
                    }
                    sljit_emit_op2(C, SLJIT_SHL32, SLJIT_S3, 0, SLJIT_R0, 0,
                                   SLJIT_IMM, n);
                }
                break;
            case ShiftType::LSR:
                if (n == 0) {  // LSR #0 == LSR #32
                    if (need_carry) {
                        sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_R0, 0,
                                       SLJIT_IMM, 31);
                    }
                    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S3, 0, SLJIT_IMM, 0);
                } else {
                    if (need_carry) {
                        sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_R0, 0,
                                       SLJIT_IMM, n - 1);
                        sljit_emit_op2(C, SLJIT_AND32, SLJIT_S4, 0, SLJIT_S4, 0,
                                       SLJIT_IMM, 1);
                    }
                    sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S3, 0, SLJIT_R0, 0,
                                   SLJIT_IMM, n);
                }
                break;
            case ShiftType::ASR:
                if (n == 0) {  // ASR #0 == ASR #32 → sign-fill
                    if (need_carry) {
                        sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_R0, 0,
                                       SLJIT_IMM, 31);
                    }
                    sljit_emit_op2(C, SLJIT_ASHR32, SLJIT_S3, 0, SLJIT_R0, 0,
                                   SLJIT_IMM, 31);
                } else {
                    if (need_carry) {
                        sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_R0, 0,
                                       SLJIT_IMM, n - 1);
                        sljit_emit_op2(C, SLJIT_AND32, SLJIT_S4, 0, SLJIT_S4, 0,
                                       SLJIT_IMM, 1);
                    }
                    sljit_emit_op2(C, SLJIT_ASHR32, SLJIT_S3, 0, SLJIT_R0, 0,
                                   SLJIT_IMM, n);
                }
                break;
            case ShiftType::ROR:  // n > 0 (n == 0 handled as RRX above)
                // val = (Rm >> n) | (Rm << (32-n)); carry = val>>31.
                sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_R1, 0, SLJIT_R0, 0,
                               SLJIT_IMM, n);
                sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R2, 0, SLJIT_R0, 0,
                               SLJIT_IMM, 32u - n);
                sljit_emit_op2(C, SLJIT_OR32, SLJIT_S3, 0, SLJIT_R1, 0,
                               SLJIT_R2, 0);
                if (need_carry)
                    sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_S3, 0,
                                   SLJIT_IMM, 31);
                break;
            case ShiftType::RRX:  // unreachable (is_rrx above)
                break;
        }
    }

    // S2 = Rn (ops other than MOV/MVN).
    if (uses_rn(op)) {
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_S2, 0,
                       SLJIT_MEM1(SLJIT_S0), kRegOff(ins.rn));
    }

    // S1 = result (32-bit). Op2 lives in S3.
    switch (op) {
        case IrOp::AND: case IrOp::TST:
            sljit_emit_op2(C, SLJIT_AND32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0);
            break;
        case IrOp::EOR: case IrOp::TEQ:
            sljit_emit_op2(C, SLJIT_XOR32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0);
            break;
        case IrOp::ORR:
            sljit_emit_op2(C, SLJIT_OR32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0);
            break;
        case IrOp::BIC:  // Rn & ~Op2
            sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0, SLJIT_S3, 0,
                           SLJIT_IMM, static_cast<sljit_sw>(0xFFFFFFFFu));
            sljit_emit_op2(C, SLJIT_AND32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_R0, 0);
            break;
        case IrOp::ADD: case IrOp::CMN:
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0);
            break;
        case IrOp::SUB: case IrOp::CMP:
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0);
            break;
        case IrOp::RSB:  // Op2 - Rn
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S3, 0, SLJIT_S2, 0);
            break;
        case IrOp::MOV:
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_S1, 0, SLJIT_S3, 0);
            break;
        case IrOp::MVN:  // ~Op2
            sljit_emit_op2(C, SLJIT_XOR32, SLJIT_S1, 0, SLJIT_S3, 0,
                           SLJIT_IMM, static_cast<sljit_sw>(0xFFFFFFFFu));
            break;
        case IrOp::ADC:  // Rn + Op2 + cpsr_c()
            emit_cpsr_c(SLJIT_R0);
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0);
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_R0, 0);
            break;
        case IrOp::SBC:  // Rn - Op2 - (1 - cpsr_c())  ==  Rn - Op2 - 1 + c
            emit_cpsr_c(SLJIT_R0);
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0);
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_IMM, 1);
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_R0, 0);
            break;
        case IrOp::RSC:  // Op2 - Rn - 1 + c
            emit_cpsr_c(SLJIT_R0);
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S3, 0, SLJIT_S2, 0);
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_IMM, 1);
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_R0, 0);
            break;
        default:
            sljit_free_compiler(C);
            return SljitFn{};
    }

    // Writeback (non-test) BEFORE the flag call (which clobbers scratch regs).
    if (!test) {
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(ins.rd),
                       SLJIT_S1, 0);
    }

    // Flags — reuse the exact C-path helpers (parity by construction).
    if (ins.set_flags) {
        if (logical) {
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);  // result
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S4, 0);  // carry
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(32, 32),
                             SLJIT_IMM, addr_of((const void*)&arm_set_nzc_logic));
        } else {
            // a, b operands. RSB/RSC swap Rn and Op2 (result = Op2 - Rn).
            const bool swap = (op == IrOp::RSB || op == IrOp::RSC);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           swap ? SLJIT_S3 : SLJIT_S2, 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                           swap ? SLJIT_S2 : SLJIT_S3, 0);
            if (op == IrOp::ADC || op == IrOp::SBC || op == IrOp::RSC) {
                // arm_set_nzcv_adc/sbc(a, b, c_in, result). cpsr is unchanged
                // until the helper runs, so recompute cpsr_c() here.
                emit_cpsr_c(SLJIT_R2);                                   // c_in
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S1, 0);  // result
                const void* helper = (op == IrOp::ADC)
                    ? (const void*)&arm_set_nzcv_adc
                    : (const void*)&arm_set_nzcv_sbc;
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS4V(32, 32, 32, 32),
                                 SLJIT_IMM, addr_of(helper));
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_S1, 0);  // result
                const bool add = (op == IrOp::ADD || op == IrOp::CMN);
                const void* helper = add ? (const void*)&arm_set_nzcv_add
                                         : (const void*)&arm_set_nzcv_sub;
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(32, 32, 32),
                                 SLJIT_IMM, addr_of(helper));
            }
        }
    }

    // Cycle tick: DP fixed cost is 1S (no register-shift surcharge — declined —
    // and no PC write). One runtime_tick at the boundary, as the C epilogue does.
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
