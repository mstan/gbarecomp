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

// Returns nullptr if the instruction body is lowerable, else a short stable tag
// naming the exact decline condition. This is the ONE place the gate logic
// lives; sljit_supports() and sljit_decline_reason() both route through it, and
// emit_one_body must lower exactly what this accepts.
const char* body_decline_reason(const Instr& ins) {
    if (is_dp_op(ins.op)) {
        if (ins.op2.kind == Op2::Kind::Shifted) {
            if (ins.op2.shifted.by_register) {
                // Register-controlled shift IS supported (emit_dp dispatches to
                // the arm_shift_<type> helpers), EXCEPT: R15 as the shifted value
                // (reads as PC+12) or as the count register Rs (read_reg+4) —
                // both rare/unpredictable — and the impossible reg-form RRX.
                if (ins.op2.shifted.rm == 15) return "dp:reg-shift-pc-rm";
                if (ins.op2.shifted.imm_or_rs == 15) return "dp:reg-shift-pc-rs";
                if (ins.op2.shifted.type == ShiftType::RRX) return "dp:reg-shift-rrx";
            }
            // R15 as the shifted Rm (immediate-shift only) IS supported:
            // emit_dp bakes the pc+8/pc+4 pipeline value.
        }
        if (!is_test_op(ins.op) && ins.rd == 15) {
            // PC-write DP (mov pc, lr / computed jump — incl. the add pc,pc,rN
            // jump-table form now that R15-as-operand is baked) IS supported —
            // see emit_dp. But S=1 (MOVS pc = exception return / SPSR restore)
            // needs privileged-mode SPSR handling → later.
            if (ins.set_flags) return "dp:S=1-pc-write";
        }
        // R15 as Rn (add rd,pc,#imm / ADR, incl. the THUMB word-align) IS
        // supported: emit_dp bakes the pipeline value.
        return nullptr;
    }
    if (is_mem_op(ins.op)) {
        if (ins.rd == 15) return "mem:pc-dest";       // PC dest/source → later
        if (ins.mem.rn == 15) {
            // PC-relative literal load (LDR rd, [pc, #imm]): supported as the
            // pure pre-indexed, immediate-offset, no-writeback form only — the
            // base is baked as the pc-pipeline constant in emit_mem and never
            // updated, so no PC write occurs. A writeback / post-index / store
            // / register-offset with a PC base would write or index off R15:
            // unpredictable and not the literal-pool idiom → later.
            if (!is_load(ins.op)) return "mem:pc-base-store";
            if (ins.mem.by_register) return "mem:pc-base-reg-off";
            if (!ins.mem.pre_indexed || ins.mem.writeback) return "mem:pc-base-wb";
            return nullptr;
        }
        if (ins.mem.by_register) {
            if (ins.mem.reg_offset.rm == 15) return "mem:reg-off-pc";
            if (ins.mem.reg_offset.type != ShiftType::LSL) return "mem:reg-off-nonlsl";
        }
        return nullptr;
    }
    if (is_mul_op(ins.op)) {  // R15 is unpredictable as a multiply operand
        if (ins.rd == 15 || ins.rm == 15 || ins.rs == 15) return "mul:pc-operand";
        if (ins.op == IrOp::MLA && ins.rn == 15) return "mul:pc-operand";
        return nullptr;
    }
    if (is_block_op(ins.op)) {
        const auto& b = ins.block;
        if (b.rn == 15) return "block:pc-base";      // PC base → unpredictable, later
        if (b.s_bit) return "block:s-bit";           // user-bank / SPSR / exc-return → later
        if (b.reg_list == 0) return "block:empty-list";  // empty-list 0x40-stride corner → later
        return nullptr;  // LDM-with-PC (return idiom) IS supported — see emit_block_transfer
    }
    if (is_branch_op(ins.op)) {
        if (ins.op == IrOp::BX && ins.rm == 15) return "branch:bx-pc";  // BX PC unpredictable
        return nullptr;
    }
    return "unhandled-op";
}

}  // namespace

const char* sljit_decline_reason(const Instr& ins) {
    if (ins.is_undefined) return "undefined";
    return body_decline_reason(ins);  // conditional exec is handled by emit_one_body
}

bool sljit_supports(const Instr& ins) {
    return sljit_decline_reason(ins) == nullptr;
}

}  // namespace armv4t
