// arm_sljit.cpp — see arm_sljit.h.
//
// COVERAGE (grows under the L1 JIT harness, one class at a time):
//   P3:        16 data-processing ops, IMMEDIATE Op2, cond AL, no PC.
//   P4 step 1: + SHIFTED Op2 (immediate count) + ADC/SBC/RSC (carry-in).
//   P4 step 2: + memory LDR/STR/LDRB/STRB/LDRH/STRH/LDRSB (immediate or
//                LSL-register offset; pre/post-index; writeback; misaligned
//                LDR/LDRH rotation; runtime_mem_cycles timing).
// Still DECLINED (precision over recall → caller runs gcc / interpreter):
//   register-COUNT shifts, non-LSL register offsets, R15 as operand/dest,
//   LDRSH (misaligned-byte branch), conditional execution, branches, LDM/STM,
//   multiplies, PSR, SWI. Each lands later, gated by the harness.
//
// Parity is by construction: value/carry/address math mirrors arm_codegen.cpp,
// and flags reuse the EXACT C-path helpers — never re-derived here. The shard
// runs in-process, so it reaches host state by baking the addresses of the
// runtime ABI symbols (g_cpu, bus_*, arm_*, runtime_*) directly.

#include "arm_sljit.h"

#include <cstdint>

#include "runtime_arm.h"
#include "sljitLir.h"

namespace armv4t {

namespace {

constexpr sljit_sw kRegOff(unsigned r) { return static_cast<sljit_sw>(r) * 4; }
constexpr sljit_sw kCpsrOff = 16 * 4;
constexpr sljit_sw kM3 = static_cast<sljit_sw>(0xFFFFFFFCu);  // ~3
constexpr sljit_sw kM1 = static_cast<sljit_sw>(0xFFFFFFFEu);  // ~1

inline sljit_sw addr_of(const void* p) {
    return static_cast<sljit_sw>(reinterpret_cast<uintptr_t>(p));
}

bool is_dp_op(IrOp op) {
    switch (op) {
        case IrOp::AND: case IrOp::EOR: case IrOp::SUB: case IrOp::RSB:
        case IrOp::ADD: case IrOp::ADC: case IrOp::SBC: case IrOp::RSC:
        case IrOp::ORR: case IrOp::MOV: case IrOp::BIC:
        case IrOp::MVN: case IrOp::TST: case IrOp::TEQ: case IrOp::CMP:
        case IrOp::CMN:
            return true;
        default: return false;
    }
}
bool is_mem_op(IrOp op) {
    switch (op) {
        case IrOp::LDR: case IrOp::STR: case IrOp::LDRB: case IrOp::STRB:
        case IrOp::LDRH: case IrOp::STRH: case IrOp::LDRSB:
            return true;  // LDRSH declined (misaligned-byte branch)
        default: return false;
    }
}
bool is_load(IrOp op) {
    return op == IrOp::LDR || op == IrOp::LDRB || op == IrOp::LDRH ||
           op == IrOp::LDRSB || op == IrOp::LDRSH;
}
bool is_mul_op(IrOp op) {
    // 32-bit forms only; 64-bit long multiplies (UMULL/UMLAL/SMULL/SMLAL)
    // need branchless 64-bit N/Z flag derivation — declined until added.
    return op == IrOp::MUL || op == IrOp::MLA;
}
unsigned access_width(IrOp op) {
    switch (op) {
        case IrOp::LDRB: case IrOp::STRB: case IrOp::LDRSB: return 1;
        case IrOp::LDRH: case IrOp::STRH: case IrOp::LDRSH: return 2;
        default: return 4;
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
        default: return false;
    }
}
bool uses_rn(IrOp op) { return !(op == IrOp::MOV || op == IrOp::MVN); }

// cpsr_c() (bit 29 of cpsr) -> dst. Assumes S0 == &g_cpu.
void emit_cpsr_c(struct sljit_compiler* C, sljit_s32 dst) {
    sljit_emit_op1(C, SLJIT_MOV_U32, dst, 0, SLJIT_MEM1(SLJIT_S0), kCpsrOff);
    sljit_emit_op2(C, SLJIT_LSHR32, dst, 0, dst, 0, SLJIT_IMM, 29);
    sljit_emit_op2(C, SLJIT_AND32, dst, 0, dst, 0, SLJIT_IMM, 1);
}

// ── Data processing. S0=&g_cpu; S1=result, S2=Rn, S3=Op2, S4=Op2 carry. ──
bool emit_dp(struct sljit_compiler* C, const Instr& ins) {
    const IrOp op = ins.op;
    const bool test = is_test_op(op);
    const bool logical = is_logical_op(op);
    const bool need_carry = ins.set_flags && logical;

    // Op2 value -> S3 (carry -> S4 when need_carry).
    if (ins.op2.kind == Op2::Kind::Imm) {
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S3, 0, SLJIT_IMM, ins.op2.imm_value);
        if (need_carry) {
            if (ins.op2.imm_carry_out == 2u) emit_cpsr_c(C, SLJIT_S4);
            else sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S4, 0, SLJIT_IMM,
                                static_cast<sljit_sw>(ins.op2.imm_carry_out & 1u));
        }
    } else {
        const auto& sr = ins.op2.shifted;
        const unsigned n = sr.imm_or_rs;
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_S0), kRegOff(sr.rm));
        const bool rrx = (sr.type == ShiftType::RRX) ||
                         (sr.type == ShiftType::ROR && n == 0);
        if (rrx) {
            if (need_carry)
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_S4, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
            emit_cpsr_c(C, SLJIT_R1);
            sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 31);
            sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
            sljit_emit_op2(C, SLJIT_OR32, SLJIT_S3, 0, SLJIT_S3, 0, SLJIT_R1, 0);
        } else switch (sr.type) {
            case ShiftType::LSL:
                if (n == 0) {
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S3, 0, SLJIT_R0, 0);
                    if (need_carry) emit_cpsr_c(C, SLJIT_S4);
                } else {
                    if (need_carry) {
                        sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_R0, 0, SLJIT_IMM, 32u - n);
                        sljit_emit_op2(C, SLJIT_AND32, SLJIT_S4, 0, SLJIT_S4, 0, SLJIT_IMM, 1);
                    }
                    sljit_emit_op2(C, SLJIT_SHL32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_IMM, n);
                }
                break;
            case ShiftType::LSR:
                if (n == 0) {
                    if (need_carry) sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_R0, 0, SLJIT_IMM, 31);
                    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S3, 0, SLJIT_IMM, 0);
                } else {
                    if (need_carry) {
                        sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_R0, 0, SLJIT_IMM, n - 1);
                        sljit_emit_op2(C, SLJIT_AND32, SLJIT_S4, 0, SLJIT_S4, 0, SLJIT_IMM, 1);
                    }
                    sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_IMM, n);
                }
                break;
            case ShiftType::ASR:
                if (n == 0) {
                    if (need_carry) sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_R0, 0, SLJIT_IMM, 31);
                    sljit_emit_op2(C, SLJIT_ASHR32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_IMM, 31);
                } else {
                    if (need_carry) {
                        sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_R0, 0, SLJIT_IMM, n - 1);
                        sljit_emit_op2(C, SLJIT_AND32, SLJIT_S4, 0, SLJIT_S4, 0, SLJIT_IMM, 1);
                    }
                    sljit_emit_op2(C, SLJIT_ASHR32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_IMM, n);
                }
                break;
            case ShiftType::ROR:  // n > 0
                sljit_emit_op2(C, SLJIT_ROTR32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_IMM, n);
                if (need_carry) sljit_emit_op2(C, SLJIT_LSHR32, SLJIT_S4, 0, SLJIT_S3, 0, SLJIT_IMM, 31);
                break;
            case ShiftType::RRX: break;  // unreachable (rrx above)
        }
    }

    if (uses_rn(op))
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_S2, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(ins.rn));

    switch (op) {
        case IrOp::AND: case IrOp::TST:
            sljit_emit_op2(C, SLJIT_AND32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0); break;
        case IrOp::EOR: case IrOp::TEQ:
            sljit_emit_op2(C, SLJIT_XOR32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0); break;
        case IrOp::ORR:
            sljit_emit_op2(C, SLJIT_OR32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0); break;
        case IrOp::BIC:
            sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0, SLJIT_S3, 0, SLJIT_IMM,
                           static_cast<sljit_sw>(0xFFFFFFFFu));
            sljit_emit_op2(C, SLJIT_AND32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_R0, 0); break;
        case IrOp::ADD: case IrOp::CMN:
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0); break;
        case IrOp::SUB: case IrOp::CMP:
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0); break;
        case IrOp::RSB:
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S3, 0, SLJIT_S2, 0); break;
        case IrOp::MOV:
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_S1, 0, SLJIT_S3, 0); break;
        case IrOp::MVN:
            sljit_emit_op2(C, SLJIT_XOR32, SLJIT_S1, 0, SLJIT_S3, 0, SLJIT_IMM,
                           static_cast<sljit_sw>(0xFFFFFFFFu)); break;
        case IrOp::ADC:
            emit_cpsr_c(C, SLJIT_R0);
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0);
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_R0, 0); break;
        case IrOp::SBC:
            emit_cpsr_c(C, SLJIT_R0);
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S2, 0, SLJIT_S3, 0);
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_IMM, 1);
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_R0, 0); break;
        case IrOp::RSC:
            emit_cpsr_c(C, SLJIT_R0);
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S3, 0, SLJIT_S2, 0);
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_IMM, 1);
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_R0, 0); break;
        default: return false;
    }

    if (!test)
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(ins.rd), SLJIT_S1, 0);

    if (ins.set_flags) {
        if (logical) {
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S4, 0);
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(32, 32),
                             SLJIT_IMM, addr_of((const void*)&arm_set_nzc_logic));
        } else {
            const bool swap = (op == IrOp::RSB || op == IrOp::RSC);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, swap ? SLJIT_S3 : SLJIT_S2, 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, swap ? SLJIT_S2 : SLJIT_S3, 0);
            if (op == IrOp::ADC || op == IrOp::SBC || op == IrOp::RSC) {
                emit_cpsr_c(C, SLJIT_R2);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S1, 0);
                const void* h = (op == IrOp::ADC) ? (const void*)&arm_set_nzcv_adc
                                                  : (const void*)&arm_set_nzcv_sbc;
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS4V(32, 32, 32, 32),
                                 SLJIT_IMM, addr_of(h));
            } else {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_S1, 0);
                const bool add = (op == IrOp::ADD || op == IrOp::CMN);
                const void* h = add ? (const void*)&arm_set_nzcv_add
                                    : (const void*)&arm_set_nzcv_sub;
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(32, 32, 32),
                                 SLJIT_IMM, addr_of(h));
            }
        }
    }

    // DP fixed cost = 1S (no register-count-shift surcharge; no PC write).
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 1);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32),
                     SLJIT_IMM, addr_of((const void*)&runtime_tick));
    return true;
}

// ── Memory. S0=&g_cpu; S1=ea, S2=post (writeback addr), S3=value, S4=cyc. ──
bool emit_mem(struct sljit_compiler* C, const Instr& ins) {
    const IrOp op = ins.op;
    const auto& mem = ins.mem;

    // base -> R0, offset -> R1.
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(mem.rn));
    if (!mem.by_register) {
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, mem.imm_offset);
    } else {  // register offset, LSL imm count (others declined in supports)
        const unsigned n = mem.reg_offset.imm_or_rs;
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R2, 0,
                       SLJIT_MEM1(SLJIT_S0), kRegOff(mem.reg_offset.rm));
        if (n == 0) sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_R2, 0);
        else sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R1, 0, SLJIT_R2, 0, SLJIT_IMM, n);
    }
    // post = base +/- off (S2); ea = pre_indexed ? post : base (S1).
    sljit_emit_op2(C, mem.add ? SLJIT_ADD32 : SLJIT_SUB32, SLJIT_S2, 0,
                   SLJIT_R0, 0, SLJIT_R1, 0);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S1, 0,
                   mem.pre_indexed ? SLJIT_S2 : SLJIT_R0, 0);

    // cyc = instr_cycle_base + runtime_mem_cycles(ea, width, 0).
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S4, 0, SLJIT_IMM,
                   static_cast<sljit_sw>(instr_cycle_base(op)));
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
                   static_cast<sljit_sw>(access_width(op)));
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 0);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(32, 32, 32, 32),
                     SLJIT_IMM, addr_of((const void*)&runtime_mem_cycles));
    sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S4, 0, SLJIT_S4, 0, SLJIT_R0, 0);

    if (is_load(op)) {
        switch (op) {
            case IrOp::LDR:  // misaligned word load rotates by (ea&3)*8
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_S1, 0, SLJIT_IMM, kM3);
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32),
                                 SLJIT_IMM, addr_of((const void*)&bus_read_u32));
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_S1, 0, SLJIT_IMM, 3);
                sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 3);
                sljit_emit_op2(C, SLJIT_ROTR32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                break;
            case IrOp::LDRB:
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32),
                                 SLJIT_IMM, addr_of((const void*)&bus_read_u8));
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_IMM, 0xFF);
                break;
            case IrOp::LDRH:  // misaligned halfword rotates by (ea&1)*8
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_S1, 0, SLJIT_IMM, kM1);
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32),
                                 SLJIT_IMM, addr_of((const void*)&bus_read_u16));
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xFFFF);
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_S1, 0, SLJIT_IMM, 1);
                sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 3);
                sljit_emit_op2(C, SLJIT_ROTR32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                break;
            case IrOp::LDRSB:  // sign-extend low byte: (x<<24)>>24 (arithmetic)
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32),
                                 SLJIT_IMM, addr_of((const void*)&bus_read_u8));
                sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 24);
                sljit_emit_op2(C, SLJIT_ASHR32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_IMM, 24);
                break;
            default: return false;
        }
        // Rn writeback (post-index or explicit W), Rd wins on a load (rn != rd).
        if ((mem.writeback || !mem.pre_indexed) && mem.rn != ins.rd)
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(mem.rn),
                           mem.pre_indexed ? SLJIT_S1 : SLJIT_S2, 0);
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(ins.rd), SLJIT_S3, 0);
    } else {
        // value to store = R[rd] (rd != 15 enforced) -> S3.
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_S3, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(ins.rd));
        switch (op) {
            case IrOp::STR:
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_S1, 0, SLJIT_IMM, kM3);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S3, 0);
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(32, 32),
                                 SLJIT_IMM, addr_of((const void*)&bus_write_u32));
                break;
            case IrOp::STRB:
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_S3, 0, SLJIT_IMM, 0xFF);
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(32, 32),
                                 SLJIT_IMM, addr_of((const void*)&bus_write_u8));
                break;
            case IrOp::STRH:
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_S1, 0, SLJIT_IMM, kM1);
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_S3, 0, SLJIT_IMM, 0xFFFF);
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(32, 32),
                                 SLJIT_IMM, addr_of((const void*)&bus_write_u16));
                break;
            default: return false;
        }
        if (mem.writeback || !mem.pre_indexed)
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(mem.rn),
                           mem.pre_indexed ? SLJIT_S1 : SLJIT_S2, 0);
    }

    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S4, 0);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32),
                     SLJIT_IMM, addr_of((const void*)&runtime_tick));
    return true;
}

// ── Multiply (MUL/MLA). S0=&g_cpu; S1=result, S4=cyc. ──
bool emit_mul(struct sljit_compiler* C, const Instr& ins) {
    const unsigned rd = ins.rd, rn = ins.rn, rm = ins.rm, rs = ins.rs;

    // cyc = base(1) + runtime_mul_cycles(operand, signed=1, extra). The
    // multiplier operand is Rm on THUMB MUL, else Rs; MLA adds extra=1.
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S4, 0, SLJIT_IMM,
                   static_cast<sljit_sw>(instr_cycle_base(ins.op)));
    const unsigned cyc_operand =
        (ins.op == IrOp::MUL && ins.thumb) ? rm : rs;
    const unsigned extra = (ins.op == IrOp::MLA) ? 1u : 0u;
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
                   SLJIT_MEM1(SLJIT_S0), kRegOff(cyc_operand));
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, 1);  // signed_variant
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM,
                   static_cast<sljit_sw>(extra));
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(32, 32, 32, 32),
                     SLJIT_IMM, addr_of((const void*)&runtime_mul_cycles));
    sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S4, 0, SLJIT_S4, 0, SLJIT_R0, 0);

    // result = Rm * Rs (+ Rn for MLA), low 32 bits.
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(rm));
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(rs));
    sljit_emit_op2(C, SLJIT_MUL32, SLJIT_S1, 0, SLJIT_R0, 0, SLJIT_R1, 0);
    if (ins.op == IrOp::MLA) {
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(rn));
        sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_R2, 0);
    }
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(rd), SLJIT_S1, 0);

    if (ins.set_flags) {  // MUL/MLA set N/Z only (C unchanged on ARMv4T).
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32),
                         SLJIT_IMM, addr_of((const void*)&arm_set_nz));
    }

    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S4, 0);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32),
                     SLJIT_IMM, addr_of((const void*)&runtime_tick));
    return true;
}

}  // namespace

bool sljit_supports(const Instr& ins) {
    if (ins.is_undefined) return false;
    if (ins.cond != Cond::AL) return false;  // conditional → later

    if (is_dp_op(ins.op)) {
        if (ins.op2.kind == Op2::Kind::Shifted) {
            if (ins.op2.shifted.by_register) return false;  // register count → later
            if (ins.op2.shifted.rm == 15) return false;     // PC operand → later
        }
        if (!is_test_op(ins.op) && ins.rd == 15) return false;
        if (uses_rn(ins.op) && ins.rn == 15) return false;
        return true;
    }
    if (is_mem_op(ins.op)) {
        if (ins.mem.rn == 15) return false;   // PC base → later
        if (ins.rd == 15) return false;       // PC dest/source → later
        if (ins.mem.by_register) {
            if (ins.mem.reg_offset.rm == 15) return false;
            if (ins.mem.reg_offset.type != ShiftType::LSL) return false;  // non-LSL → later
        }
        return true;
    }
    if (is_mul_op(ins.op)) {  // R15 is unpredictable as a multiply operand
        if (ins.rd == 15 || ins.rm == 15 || ins.rs == 15) return false;
        if (ins.op == IrOp::MLA && ins.rn == 15) return false;
        return true;
    }
    return false;
}

SljitFn emit_instr_sljit(const Instr& ins) {
    if (!sljit_supports(ins)) return SljitFn{};

    struct sljit_compiler* C = sljit_create_compiler(nullptr);
    if (!C) return SljitFn{};

    // S0 = &g_cpu; bodies use S1..S4 + R0..R3.
    sljit_emit_enter(C, 0, SLJIT_ARGS0V(), 4, 5, 0);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S0, 0, SLJIT_IMM, addr_of(&g_cpu));

    bool ok;
    if (is_dp_op(ins.op))       ok = emit_dp(C, ins);
    else if (is_mem_op(ins.op)) ok = emit_mem(C, ins);
    else if (is_mul_op(ins.op)) ok = emit_mul(C, ins);
    else                        ok = false;
    if (!ok) { sljit_free_compiler(C); return SljitFn{}; }

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
