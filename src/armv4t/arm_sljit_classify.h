// arm_sljit_classify.h — pure IrOp/Instr classification for the sljit emitter.
//
// These predicates have NO runtime or sljit dependency (just arm_ir.h), so they
// are shared between the runtime-dependent emitter (arm_sljit.cpp, in
// gbarecomp_sljit_emit) and the dependency-free support TU (arm_sljit_support.cpp,
// in gbarecomp_armv4t — which is why sljit_supports() is callable from anywhere
// armv4t links, e.g. the gba_recompile coverage measurement, without dragging in
// the emitter's baked runtime-symbol references).

#pragma once

#include "arm_ir.h"

namespace armv4t {
namespace sjc {

inline bool is_dp_op(IrOp op) {
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

inline bool is_mem_op(IrOp op) {
    switch (op) {
        case IrOp::LDR: case IrOp::STR: case IrOp::LDRB: case IrOp::STRB:
        case IrOp::LDRH: case IrOp::STRH: case IrOp::LDRSB:
            return true;  // LDRSH declined (misaligned-byte branch)
        default: return false;
    }
}

inline bool is_mul_op(IrOp op) { return op == IrOp::MUL || op == IrOp::MLA; }

inline bool is_block_op(IrOp op) { return op == IrOp::LDM || op == IrOp::STM; }

inline bool is_branch_op(IrOp op) {
    return op == IrOp::B || op == IrOp::BX || op == IrOp::BL ||
           op == IrOp::BL_prefix || op == IrOp::BL_suffix;
}

inline bool is_load(IrOp op) {
    return op == IrOp::LDR || op == IrOp::LDRB || op == IrOp::LDRH ||
           op == IrOp::LDRSB || op == IrOp::LDRSH;
}

inline unsigned access_width(IrOp op) {
    switch (op) {
        case IrOp::LDRB: case IrOp::STRB: case IrOp::LDRSB: return 1;
        case IrOp::LDRH: case IrOp::STRH: case IrOp::LDRSH: return 2;
        default: return 4;
    }
}

inline bool is_test_op(IrOp op) {
    return op == IrOp::TST || op == IrOp::TEQ ||
           op == IrOp::CMP || op == IrOp::CMN;
}

inline bool is_logical_op(IrOp op) {
    switch (op) {
        case IrOp::AND: case IrOp::EOR: case IrOp::ORR: case IrOp::BIC:
        case IrOp::MOV: case IrOp::MVN: case IrOp::TST: case IrOp::TEQ:
            return true;
        default: return false;
    }
}

inline bool uses_rn(IrOp op) { return !(op == IrOp::MOV || op == IrOp::MVN); }

}  // namespace sjc
}  // namespace armv4t
