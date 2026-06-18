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
//   P7:        + block transfer (LDM/STM), incl. the POP {.., pc} return idiom
//              (exits the function like a branch; SP base C-returns, else
//              dispatches). Declines s_bit / empty-list / rn==15.
//   P7b:       + PC-relative LDR literal (LDR rd, [pc, #imm], rd!=15) — base
//              baked as the pc-pipeline constant; no-writeback pre-indexed only.
// Still DECLINED (precision over recall → caller runs gcc / interpreter):
//   register-COUNT shifts, non-LSL reg offsets, LDRSH, long multiplies,
//   PSR/SWI, R15 as a value operand / PC-write loads, LDM/STM s_bit +
//   empty-list + PC base.
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
#include <unordered_map>
#include <vector>

#include "arm_sljit_classify.h"  // shared op classifiers (sjc::)
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

// Op classifiers live in arm_sljit_classify.h (shared with arm_sljit_support.cpp);
// pull them in unqualified.
using namespace sjc;

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

    if (mem.rn == 15) {
        // PC-relative literal load: the base is the pc-pipeline value, not the
        // live R15 (which emit_one_body set to ins.pc). THUMB word-aligns it.
        // Gated to the no-writeback pre-indexed form, so this base is never
        // written back (it would be a PC write).
        const uint32_t base = ins.thumb ? ((ins.pc + 4u) & ~3u) : (ins.pc + 8u);
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM,
                       static_cast<sljit_sw>(base));
    } else {
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_S0), kRegOff(mem.rn));
    }
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
    sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S5, 0, SLJIT_S5, 0, SLJIT_R0, 0);  // += mem cycles

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
    sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S5, 0, SLJIT_S5, 0, SLJIT_R0, 0);  // += mul cycles

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

// ── Block transfer (LDM/STM). S0=&g_cpu; S1=running address, S2=final base
// (writeback value), S5=cyc. Mirrors arm_codegen::emit_block_transfer and the
// interpreter's LDM/STM execute path exactly: the base is read ONCE, registers
// transfer in ascending order to ascending addresses, the first access is
// non-sequential (N) and the rest sequential (S) — each access folds its
// runtime_mem_cycles into S5. Declined upstream (arm_sljit_support): s_bit
// (user-bank/SPSR/exception-return), an empty reg_list (the 0x40-stride
// corner), and rn==15.
//
// STM never writes R15. LDM with R15 in the list is the function-return idiom
// (POP {.., pc}): it adds the non-branch pipeline refill (+2), ticks the
// accumulated cost, then exits the whole function — when the base is SP it
// first asks runtime_call_should_return (C-return to the caller's body),
// otherwise (a computed jump) it runtime_dispatches. That exit takes ret_jumps,
// exactly like a branch; non-PC LDM/STM fall through to the shared epilogue.
//
// Like the existing emit_mem store path, this does NOT emit runtime_trace_event:
// the sljit backend uniformly omits the watchpoint memory-write event (the
// gcc-DLL / interpreter tiers carry production tracing). ──
bool emit_block_transfer(struct sljit_compiler* C, const Instr& ins,
                         std::vector<struct sljit_jump*>& ret_jumps) {
    const auto& blk = ins.block;
    const uint16_t list = blk.reg_list;

    unsigned n = 0;
    for (int r = 0; r < 16; ++r) if (list & (1u << r)) ++n;
    const bool pc_in_list = (list & (1u << 15)) != 0;
    const bool base_in_list = (list & (1u << blk.rn)) != 0;

    // base = g_cpu.R[rn] (read once). S1 = start address; S2 = final base.
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
                   SLJIT_MEM1(SLJIT_S0), kRegOff(blk.rn));
    if (blk.add) {
        if (blk.pre_indexed)
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_R0, 0, SLJIT_IMM, 4);
        else
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_S1, 0, SLJIT_R0, 0);
        sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S2, 0, SLJIT_R0, 0, SLJIT_IMM, 4 * n);
    } else {
        const unsigned back = blk.pre_indexed ? 4u * n : 4u * (n - 1u);
        sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S1, 0, SLJIT_R0, 0, SLJIT_IMM, back);
        sljit_emit_op2(C, SLJIT_SUB32, SLJIT_S2, 0, SLJIT_R0, 0, SLJIT_IMM, 4 * n);
    }

    // LDM that writes PC incurs the non-branch pipeline refill (+2). STM never
    // writes PC; emit_one_body seeded S5 with instr_cycle_base only.
    if (blk.load && pc_in_list)
        sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S5, 0, SLJIT_S5, 0, SLJIT_IMM, 2);

    bool first = true;
    for (int r = 0; r < 16; ++r) {
        if (!(list & (1u << r))) continue;

        // cyc += runtime_mem_cycles(addr & ~3, 4, first ? 0 : 1)
        sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_S1, 0, SLJIT_IMM, kM3);
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, 4);
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, first ? 0 : 1);
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(32, 32, 32, 32),
                         SLJIT_IMM, addr_of((const void*)&runtime_mem_cycles));
        sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S5, 0, SLJIT_S5, 0, SLJIT_R0, 0);
        first = false;

        if (blk.load) {
            sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_S1, 0, SLJIT_IMM, kM3);
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32),
                             SLJIT_IMM, addr_of((const void*)&bus_read_u32));
            // PC load forces bit 0 clear (ARMv4T LDM has no interworking).
            if (r == 15)
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, kM1);
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(r),
                           SLJIT_R0, 0);
        } else {
            sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_S1, 0, SLJIT_IMM, kM3);
            if (r == 15) {
                // STM stores the pipeline-ahead PC value (pc+12 ARM; the
                // word-aligned pc+4+4 in THUMB). Statically known.
                const uint32_t pcv = ins.thumb ? (((ins.pc + 4u) & ~2u) + 4u)
                                               : (ins.pc + 12u);
                sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
                               static_cast<sljit_sw>(pcv));
            } else {
                // ARM "store base when not first in list" corner: a writeback
                // STM stores the NEW base for Rn when lower-numbered regs precede
                // it; otherwise the register's current value.
                const bool wb_base = blk.writeback &&
                                     static_cast<unsigned>(r) == blk.rn &&
                                     (list & ((1u << r) - 1u)) != 0;
                if (wb_base)
                    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S2, 0);
                else
                    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0,
                                   SLJIT_MEM1(SLJIT_S0), kRegOff(r));
            }
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(32, 32),
                             SLJIT_IMM, addr_of((const void*)&bus_write_u32));
        }

        sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_IMM, 4);
    }

    // Writeback (suppressed for LDM when the base register was itself loaded).
    if (blk.writeback && !(blk.load && base_in_list))
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(blk.rn),
                       SLJIT_S2, 0);

    // LDM-with-PC exits the function. Tick the cost once, then C-return (SP
    // base, matching a pending direct-call return) or dispatch the popped PC.
    if (blk.load && pc_in_list) {
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S5, 0);
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32),
                         SLJIT_IMM, addr_of((const void*)&runtime_tick));
        if (blk.rn == 13) {
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_S0), kRegOff(15));
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32),
                             SLJIT_IMM, addr_of((const void*)&runtime_call_should_return));
            sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xFF);
            ret_jumps.push_back(
                sljit_emit_cmp(C, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0));
        }
        sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(15));
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32),
                         SLJIT_IMM, addr_of((const void*)&runtime_dispatch));
        ret_jumps.push_back(sljit_emit_jump(C, SLJIT_JUMP));
    }
    return true;
}

// Whole-function emit context: the function's PC range and the labels emitted
// at backward-branch targets (pc → label). Null for the single-instruction and
// block paths, where every transfer is external (dispatch).
struct FnCtx {
    uint32_t lo;
    uint32_t hi;
    std::unordered_map<uint32_t, struct sljit_label*>* labels;
};

// ── Branches. S0=&g_cpu; S1=target (BX/BL_suffix); S5=exec cost (set by
// emit_one_body). Mirrors emit_branch with an EMPTY names_by_key (the overlay
// model: every transfer goes through runtime_dispatch). A taken transfer ticks
// the cost and appends a jump to ret_jumps (→ function return). Calls (BL /
// BL_suffix) may instead FALL THROUGH: after the callee C-returns to the
// pushed return address, control resumes in this function — so they zero S5
// (already ticked) and let the epilogue advance R15. A BACKWARD intra-function
// B becomes a direct sljit jump to the target's label (the loop optimization
// the C codegen does, avoiding re-dispatch recursion); everything else
// dispatches. ──
bool emit_branch_body(struct sljit_compiler* C, const Instr& ins,
                      std::vector<struct sljit_jump*>& ret_jumps,
                      const FnCtx* fn) {
    auto tick_s5 = [&]() {
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S5, 0);
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32),
                         SLJIT_IMM, addr_of((const void*)&runtime_tick));
    };
    auto call1 = [&](const void* fn) {  // void fn(R0)
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32), SLJIT_IMM, addr_of(fn));
    };

    switch (ins.op) {
        case IrOp::B: {
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, ins.branch_target);
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(15), SLJIT_R0, 0);
            tick_s5();
            // Backward intra-function branch → jump to the already-emitted label.
            if (fn && ins.branch_target >= fn->lo && ins.branch_target < fn->hi &&
                ins.branch_target < ins.pc) {
                auto it = fn->labels->find(ins.branch_target);
                if (it != fn->labels->end() && it->second) {
                    struct sljit_jump* j = sljit_emit_jump(C, SLJIT_JUMP);
                    sljit_set_label(j, it->second);
                    return true;
                }
            }
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, ins.branch_target);
            call1((const void*)&runtime_dispatch);
            ret_jumps.push_back(sljit_emit_jump(C, SLJIT_JUMP));
            return true;
        }

        case IrOp::BX: {
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_S1, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(ins.rm));
            sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_S1, 0, SLJIT_IMM, kM1);
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(15), SLJIT_R0, 0);
            tick_s5();
            if (ins.rm == 14) {
                // `bx lr`: set CPSR.T from target bit 0, then C-return if this
                // matches the pending direct-call return.
                sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), kCpsrOff);
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM,
                               static_cast<sljit_sw>(~(1u << 5)));
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_S1, 0, SLJIT_IMM, 1);
                sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 5);
                sljit_emit_op2(C, SLJIT_OR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kCpsrOff, SLJIT_R0, 0);
                sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(15));
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32),
                                 SLJIT_IMM, addr_of((const void*)&runtime_call_should_return));
                sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xFF);
                ret_jumps.push_back(sljit_emit_cmp(C, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0));
            }
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
            call1((const void*)&runtime_dispatch_with_exchange);
            ret_jumps.push_back(sljit_emit_jump(C, SLJIT_JUMP));
            return true;
        }

        case IrOp::BL_prefix:  // THUMB BL upper half: stash partial target in LR.
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, ins.branch_target);
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(14), SLJIT_R0, 0);
            return true;  // non-terminating → epilogue ticks S5 (=1), advances R15

        case IrOp::BL: {
            const uint32_t link = ins.pc + (ins.thumb ? 2u : 4u);
            const uint32_t lr = ins.thumb ? (link | 1u) : link;
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, lr);
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(14), SLJIT_R0, 0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, ins.branch_target);
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(15), SLJIT_R0, 0);
            if (ins.branch_target == link)
                return true;  // bl-to-next (get-PC): LR set, fall through
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, link & ~1u);
            call1((const void*)&runtime_call_push_return);
            tick_s5();
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S5, 0, SLJIT_IMM, 0);  // no double-tick
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, ins.branch_target);
            call1((const void*)&runtime_dispatch);
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(15));
            struct sljit_jump* fall =
                sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, link & ~1u);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, link & ~1u);
            call1((const void*)&runtime_call_cancel_return);
            ret_jumps.push_back(sljit_emit_jump(C, SLJIT_JUMP));
            sljit_set_label(fall, sljit_emit_label(C));
            return true;
        }

        case IrOp::BL_suffix: {
            // THUMB BL lower half: target = (LR + imm) & ~1; new LR = (pc+2)|1.
            const uint32_t new_lr = (ins.pc + 2u) | 1u;
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_S1, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(14));
            sljit_emit_op2(C, SLJIT_ADD32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_IMM, ins.swi_imm);
            sljit_emit_op2(C, SLJIT_AND32, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_IMM, kM1);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, new_lr);
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(14), SLJIT_R0, 0);
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(15), SLJIT_S1, 0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, new_lr & ~1u);
            call1((const void*)&runtime_call_push_return);
            tick_s5();
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S5, 0, SLJIT_IMM, 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
            call1((const void*)&runtime_dispatch);
            sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), kRegOff(15));
            struct sljit_jump* fall =
                sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, new_lr & ~1u);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, new_lr & ~1u);
            call1((const void*)&runtime_call_cancel_return);
            ret_jumps.push_back(sljit_emit_jump(C, SLJIT_JUMP));
            sljit_set_label(fall, sljit_emit_label(C));
            return true;
        }

        default: return false;
    }
}

// ── The faithful per-instruction unit (mirrors arm_codegen::emit_instr). ──
// Emits prologue (R15=pc, yield-bail, gated fingerprint), the cyc baseline,
// the conditional guard, the op body, and the epilogue (R15=next + tick). Any
// jump that should leave the whole function (the yield bail; later, branches)
// is appended to `ret_jumps` for the caller to bind to the function's return.
bool emit_one_body(struct sljit_compiler* C, const Instr& ins,
                   std::vector<struct sljit_jump*>& ret_jumps, const FnCtx* fn) {
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

    // Exec cost (cond-pass path): instr_cycle_base + surcharges. The remaining
    // surcharge cases (register-count shifts, non-block PC writes) are declined,
    // so this is the base; mem/mul ADD their runtime component, and an LDM that
    // writes PC adds its own +2 refill in emit_block_transfer. The cond-fail
    // path skips this and keeps the S5==1 fetch baseline.
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_S5, 0, SLJIT_IMM,
                   static_cast<sljit_sw>(instr_cycle_base(ins.op)));

    bool ok;
    if (is_branch_op(ins.op))     ok = emit_branch_body(C, ins, ret_jumps, fn);
    else if (is_dp_op(ins.op))    ok = emit_dp(C, ins);
    else if (is_mem_op(ins.op))   ok = emit_mem(C, ins);
    else if (is_mul_op(ins.op))   ok = emit_mul(C, ins);
    else if (is_block_op(ins.op)) ok = emit_block_transfer(C, ins, ret_jumps);
    else                          ok = false;
    if (!ok) return false;

    // Epilogue: R15 = next; runtime_tick(cyc). A taken branch jumped to the
    // function return above, so for it this is unreached; it IS the live path
    // for non-branch ops and for the cond-fail (not-taken) path, which lands
    // here with cyc == 1 (the 1S fetch).
    struct sljit_label* L_epi = sljit_emit_label(C);
    if (skip_body) sljit_set_label(skip_body, L_epi);
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, next);
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), kRegOff(15), SLJIT_R0, 0);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S5, 0);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1V(32),
                     SLJIT_IMM, addr_of((const void*)&runtime_tick));
    return true;
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

SljitFn emit_instr_sljit(const Instr& ins) {
    if (!sljit_supports(ins)) return SljitFn{};
    struct sljit_compiler* C = sljit_create_compiler(nullptr);
    if (!C) return SljitFn{};
    emit_enter_frame(C);
    std::vector<struct sljit_jump*> ret_jumps;
    if (!emit_one_body(C, ins, ret_jumps, nullptr)) { sljit_free_compiler(C); return SljitFn{}; }
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
        if (!emit_one_body(C, ins[i], ret_jumps, nullptr)) {
            sljit_free_compiler(C);
            return SljitFn{};
        }
    }
    struct sljit_label* L_ret = sljit_emit_label(C);
    for (auto* j : ret_jumps) sljit_set_label(j, L_ret);
    return finish(C);
}

SljitFn emit_function_sljit(const Instr* prog, unsigned count) {
    if (count == 0) return SljitFn{};
    for (unsigned i = 0; i < count; ++i)
        if (!sljit_supports(prog[i])) return SljitFn{};

    const uint32_t lo = prog[0].pc;
    const uint32_t hi = prog[count - 1].pc + (prog[count - 1].thumb ? 2u : 4u);

    // Pass 1: mark the targets of backward intra-function B branches (loops).
    std::unordered_map<uint32_t, struct sljit_label*> labels;
    for (unsigned i = 0; i < count; ++i) {
        const Instr& in = prog[i];
        if (in.op == IrOp::B && in.branch_target >= lo && in.branch_target < hi &&
            in.branch_target < in.pc) {
            labels.emplace(in.branch_target, nullptr);
        }
    }

    struct sljit_compiler* C = sljit_create_compiler(nullptr);
    if (!C) return SljitFn{};
    emit_enter_frame(C);
    FnCtx fnctx{lo, hi, &labels};
    std::vector<struct sljit_jump*> ret_jumps;

    // Pass 2: emit. A label is placed at the start of each backward target's
    // body (before its prologue), so a backward B re-runs the target faithfully.
    for (unsigned i = 0; i < count; ++i) {
        auto it = labels.find(prog[i].pc);
        if (it != labels.end()) it->second = sljit_emit_label(C);
        if (!emit_one_body(C, prog[i], ret_jumps, &fnctx)) {
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
