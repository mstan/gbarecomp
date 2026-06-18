// arm_sljit.cpp — see arm_sljit.h.
//
// COVERAGE (grows under the L1 JIT harness, one class at a time):
//   P3:        16 DP ops, immediate Op2, cond AL, no PC.
//   P4:        + immediate-count shifted Op2 + ADC/SBC/RSC; + memory
//              (LDR/STR/LDRB/STRB/LDRH/STRH/LDRSB); + MUL/MLA.
//   P5 step 1: emit_block_sljit composes leaves into one straight-line function.
//   P5 step 2: emit_one_body — the FAITHFUL per-instruction unit (R15 writes,
//              runtime_should_yield bail, gated runtime_insn_fp fingerprint,
//              CONDITIONAL execution via arm_cond_passes, shared-epilogue tick).
//              The single-instruction and block/function paths share it.
// Still DECLINED (precision over recall → caller runs gcc / interpreter):
//   branches/PC-writes, LDM/STM, register-COUNT shifts, non-LSL reg offsets,
//   LDRSH, long multiplies, PSR/SWI, R15 operands. (Branches + intra-function
//   labels + emit_function over an extent are the next P5 steps.)
//
// Parity is by construction: value/carry/address/flag math mirrors
// arm_codegen.cpp, and the per-instruction prologue/epilogue mirrors
// emit_instr — same R15 timing, same cyc model, same flag helpers. The shard
// runs in-process, so it reaches host state by baking the addresses of the
// runtime ABI symbols (g_cpu, bus_*, arm_*, runtime_*) directly.
//
// Registers (inside a function): S0=&g_cpu (whole function), S5=cyc accumulator
// (per instruction), S1..S4 + R0..R3 = per-instruction body scratch.

#include "arm_sljit.h"

#include <cstdint>
#include <vector>

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
bool is_mul_op(IrOp op) { return op == IrOp::MUL || op == IrOp::MLA; }
bool is_load(IrOp op) {
    return op == IrOp::LDR || op == IrOp::LDRB || op == IrOp::LDRH ||
           op == IrOp::LDRSB || op == IrOp::LDRSH;
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

// ── Data processing. S0=&g_cpu; S1=result, S2=Rn, S3=Op2, S4=Op2 carry.
// cyc is a fixed 1S (no register-count-shift surcharge; no PC write), already
// the S5 baseline set by emit_one_body, so emit_dp leaves S5 untouched. ──
bool emit_dp(struct sljit_compiler* C, const Instr& ins) {
    const IrOp op = ins.op;
    const bool test = is_test_op(op);
    const bool logical = is_logical_op(op);
    const bool need_carry = ins.set_flags && logical;

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
    return true;  // cyc = S5 baseline (1S); tick is in the shared epilogue.
}

// ── Memory. S0=&g_cpu; S1=ea, S2=post, S3=value, S5=cyc. ──
bool emit_mem(struct sljit_compiler* C, const Instr& ins) {
    const IrOp op = ins.op;
    const auto& mem = ins.mem;

    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(mem.rn));
    if (!mem.by_register) {
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, mem.imm_offset);
    } else {
        const unsigned n = mem.reg_offset.imm_or_rs;
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R2, 0,
                       SLJIT_MEM1(SLJIT_S0), kRegOff(mem.reg_offset.rm));
        if (n == 0) sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_R2, 0);
        else sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R1, 0, SLJIT_R2, 0, SLJIT_IMM, n);
    }
    sljit_emit_op2(C, mem.add ? SLJIT_ADD32 : SLJIT_SUB32, SLJIT_S2, 0,
                   SLJIT_R0, 0, SLJIT_R1, 0);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S1, 0,
                   mem.pre_indexed ? SLJIT_S2 : SLJIT_R0, 0);

    // cyc = instr_cycle_base + runtime_mem_cycles(ea, width, 0).
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
                   static_cast<sljit_sw>(access_width(op)));
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 0);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(32, 32, 32, 32),
                     SLJIT_IMM, addr_of((const void*)&runtime_mem_cycles));
    sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S5, 0, SLJIT_R0, 0, SLJIT_IMM,
                   static_cast<sljit_sw>(instr_cycle_base(op)));

    if (is_load(op)) {
        switch (op) {
            case IrOp::LDR:
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
            case IrOp::LDRH:
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_S1, 0, SLJIT_IMM, kM1);
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32),
                                 SLJIT_IMM, addr_of((const void*)&bus_read_u16));
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xFFFF);
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_S1, 0, SLJIT_IMM, 1);
                sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 3);
                sljit_emit_op2(C, SLJIT_ROTR32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                break;
            case IrOp::LDRSB:
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32),
                                 SLJIT_IMM, addr_of((const void*)&bus_read_u8));
                sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 24);
                sljit_emit_op2(C, SLJIT_ASHR32, SLJIT_S3, 0, SLJIT_R0, 0, SLJIT_IMM, 24);
                break;
            default: return false;
        }
        if ((mem.writeback || !mem.pre_indexed) && mem.rn != ins.rd)
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(mem.rn),
                           mem.pre_indexed ? SLJIT_S1 : SLJIT_S2, 0);
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(ins.rd), SLJIT_S3, 0);
    } else {
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
    return true;
}

// ── Multiply (MUL/MLA). S0=&g_cpu; S1=result, S5=cyc. ──
bool emit_mul(struct sljit_compiler* C, const Instr& ins) {
    const unsigned rd = ins.rd, rn = ins.rn, rm = ins.rm, rs = ins.rs;

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
    sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S5, 0, SLJIT_R0, 0, SLJIT_IMM,
                   static_cast<sljit_sw>(instr_cycle_base(ins.op)));

    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(rm));
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(rs));
    sljit_emit_op2(C, SLJIT_MUL32, SLJIT_S1, 0, SLJIT_R0, 0, SLJIT_R1, 0);
    if (ins.op == IrOp::MLA) {
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(rn));
        sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_R2, 0);
    }
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(rd), SLJIT_S1, 0);

    if (ins.set_flags) {
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32),
                         SLJIT_IMM, addr_of((const void*)&arm_set_nz));
    }
    return true;
}

// ── The faithful per-instruction unit (mirrors arm_codegen::emit_instr). ──
// Emits prologue (R15=pc, yield-bail, gated fingerprint), the cyc baseline,
// the conditional guard, the op body, and the epilogue (R15=next + tick). Any
// jump that should leave the whole function (the yield bail; later, branches)
// is appended to `ret_jumps` for the caller to bind to the function's return.
bool emit_one_body(struct sljit_compiler* C, const Instr& ins,
                   std::vector<struct sljit_jump*>& ret_jumps) {
    const uint32_t pc = ins.pc;
    const uint32_t next = pc + (ins.thumb ? 2u : 4u);

    // R15 = pc (exception/IRQ resume state, per the C prologue).
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, pc);
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(15), SLJIT_R0, 0);

    // if (runtime_should_yield()) return;  (bail to the function return)
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS0(32),
                     SLJIT_IMM, addr_of((const void*)&runtime_should_yield));
    sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xFF);  // bool
    ret_jumps.push_back(sljit_emit_cmp(C, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0));

    // if (g_runtime_insn_trace) runtime_insn_fp();  (gated; zero cost disarmed)
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, addr_of(&g_runtime_insn_trace));
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R1), 0);
    struct sljit_jump* skip_fp = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS0V(),
                     SLJIT_IMM, addr_of((const void*)&runtime_insn_fp));
    sljit_set_label(skip_fp, sljit_emit_label(C));

    // cyc baseline = 1S (the cond-fail cost). Body raises it (mem/mul).
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S5, 0, SLJIT_IMM, 1);

    // Conditional guard: skip the body when the condition fails.
    struct sljit_jump* skip_body = nullptr;
    if (ins.cond != Cond::AL) {
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM,
                       static_cast<sljit_sw>(static_cast<unsigned>(ins.cond)));
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32),
                         SLJIT_IMM, addr_of((const void*)&arm_cond_passes));
        skip_body = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
    }

    bool ok;
    if (is_dp_op(ins.op))       ok = emit_dp(C, ins);
    else if (is_mem_op(ins.op)) ok = emit_mem(C, ins);
    else if (is_mul_op(ins.op)) ok = emit_mul(C, ins);
    else                        ok = false;
    if (!ok) return false;

    // Epilogue: R15 = next; runtime_tick(cyc). (The cond-fail path lands here
    // with cyc == 1.)
    struct sljit_label* L_epi = sljit_emit_label(C);
    if (skip_body) sljit_set_label(skip_body, L_epi);
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, next);
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(15), SLJIT_R0, 0);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S5, 0);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32),
                     SLJIT_IMM, addr_of((const void*)&runtime_tick));
    return true;
}

bool body_supported(const Instr& ins) {
    if (is_dp_op(ins.op)) {
        if (ins.op2.kind == Op2::Kind::Shifted) {
            if (ins.op2.shifted.by_register) return false;
            if (ins.op2.shifted.rm == 15) return false;
        }
        if (!is_test_op(ins.op) && ins.rd == 15) return false;
        if (uses_rn(ins.op) && ins.rn == 15) return false;
        return true;
    }
    if (is_mem_op(ins.op)) {
        if (ins.mem.rn == 15) return false;
        if (ins.rd == 15) return false;
        if (ins.mem.by_register) {
            if (ins.mem.reg_offset.rm == 15) return false;
            if (ins.mem.reg_offset.type != ShiftType::LSL) return false;
        }
        return true;
    }
    if (is_mul_op(ins.op)) {
        if (ins.rd == 15 || ins.rm == 15 || ins.rs == 15) return false;
        if (ins.op == IrOp::MLA && ins.rn == 15) return false;
        return true;
    }
    return false;
}

// Common function frame: enter, bake S0=&g_cpu. saveds=6 covers S0..S5 (S5 is
// the cyc accumulator); scratches=4 covers R0..R3.
void emit_enter_frame(struct sljit_compiler* C) {
    sljit_emit_enter(C, 0, SLJIT_ARGS0V(), 4, 6, 0);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S0, 0, SLJIT_IMM, addr_of(&g_cpu));
}

SljitFn finish(struct sljit_compiler* C) {
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

}  // namespace

bool sljit_supports(const Instr& ins) {
    if (ins.is_undefined) return false;
    return body_supported(ins);  // conditional execution is handled by emit_one_body
}

SljitFn emit_instr_sljit(const Instr& ins) {
    if (!sljit_supports(ins)) return SljitFn{};
    struct sljit_compiler* C = sljit_create_compiler(nullptr);
    if (!C) return SljitFn{};
    emit_enter_frame(C);
    std::vector<struct sljit_jump*> ret_jumps;
    if (!emit_one_body(C, ins, ret_jumps)) { sljit_free_compiler(C); return SljitFn{}; }
    struct sljit_label* L_ret = sljit_emit_label(C);
    for (auto* j : ret_jumps) sljit_set_label(j, L_ret);
    return finish(C);
}

SljitFn emit_block_sljit(const Instr* ins, unsigned count) {
    if (count == 0) return SljitFn{};
    for (unsigned i = 0; i < count; ++i)
        if (!sljit_supports(ins[i])) return SljitFn{};

    struct sljit_compiler* C = sljit_create_compiler(nullptr);
    if (!C) return SljitFn{};
    emit_enter_frame(C);
    std::vector<struct sljit_jump*> ret_jumps;
    for (unsigned i = 0; i < count; ++i) {
        if (!emit_one_body(C, ins[i], ret_jumps)) {
            sljit_free_compiler(C);
            return SljitFn{};
        }
    }
    struct sljit_label* L_ret = sljit_emit_label(C);
    for (auto* j : ret_jumps) sljit_set_label(j, L_ret);
    return finish(C);
}

void free_sljit_fn(const SljitFn& f) {
    if (f.code) sljit_free_code(f.code, nullptr);
}

}  // namespace armv4t
