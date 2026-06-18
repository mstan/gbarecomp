// arm_sljit_support.cpp — sljit_supports(): the precision-over-recall gate.
//
// Lives in gbarecomp_armv4t (NOT gbarecomp_sljit_emit) because it has no runtime
// or sljit dependency — only the pure classifiers. That makes it linkable into
// the recompiler tool (the heal-coverage measurement) without pulling in the
// emitter's baked runtime-symbol references. The emitter (arm_sljit.cpp) and
// this file MUST agree: an instruction sljit_supports() accepts is exactly one
// emit_one_body can lower; see arm_sljit.cpp.

#include "arm_sljit.h"

#include "arm_sljit_classify.h"

namespace armv4t {

namespace {
using namespace sjc;

bool body_supported(const Instr& ins) {
    if (is_dp_op(ins.op)) {
        if (ins.op2.kind == Op2::Kind::Shifted) {
            if (ins.op2.shifted.by_register) return false;  // register count → later
            if (ins.op2.shifted.rm == 15) return false;     // PC operand → later
        }
        if (!is_test_op(ins.op) && ins.rd == 15) {
            // PC-write DP (mov pc, lr / computed jump) IS supported — see
            // emit_dp. But S=1 (MOVS pc = exception return / SPSR restore)
            // needs privileged-mode SPSR handling → later.
            if (ins.set_flags) return false;
        }
        if (uses_rn(ins.op) && ins.rn == 15) return false;
        return true;
    }
    if (is_mem_op(ins.op)) {
        if (ins.rd == 15) return false;       // PC dest/source → later
        if (ins.mem.rn == 15) {
            // PC-relative literal load (LDR rd, [pc, #imm]): supported as the
            // pure pre-indexed, immediate-offset, no-writeback form only — the
            // base is baked as the pc-pipeline constant in emit_mem and never
            // updated, so no PC write occurs. A writeback / post-index / store
            // / register-offset with a PC base would write or index off R15:
            // unpredictable and not the literal-pool idiom → later.
            if (!is_load(ins.op)) return false;
            if (ins.mem.by_register) return false;
            if (!ins.mem.pre_indexed || ins.mem.writeback) return false;
            return true;
        }
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
    if (is_block_op(ins.op)) {
        const auto& b = ins.block;
        if (b.rn == 15) return false;        // PC base → unpredictable, later
        if (b.s_bit) return false;           // user-bank / SPSR / exc-return → later
        if (b.reg_list == 0) return false;   // empty-list 0x40-stride corner → later
        return true;  // LDM-with-PC (return idiom) IS supported — see emit_block_transfer
    }
    if (is_branch_op(ins.op)) {
        if (ins.op == IrOp::BX && ins.rm == 15) return false;  // BX PC unpredictable
        return true;
    }
    return false;
}

}  // namespace

bool sljit_supports(const Instr& ins) {
    if (ins.is_undefined) return false;
    return body_supported(ins);  // conditional execution is handled by emit_one_body
}

}  // namespace armv4t
