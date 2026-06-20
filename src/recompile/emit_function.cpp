// emit_function.cpp — see emit_function.h.
//
// Verbatim extraction of the per-function emitter that lived in
// tools/gba_recompile/main.cpp. The only change vs the original is that
// output goes to a std::string (via a printf-style appender) instead of a
// FILE* — the format strings are identical, so the bytes are identical.

#include "emit_function.h"

#include <cstdarg>
#include <unordered_set>

#include "arm_codegen.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include "arm_ir.h"

namespace gbarecomp {

namespace {

// printf-style append with the exact semantics of std::fprintf, so the
// emitted bytes match the offline tool's historical output exactly.
void appendf(std::string& out, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (static_cast<std::size_t>(n) < sizeof(buf)) {
        out.append(buf, static_cast<std::size_t>(n));
        return;
    }
    std::string big(static_cast<std::size_t>(n) + 1u, '\0');
    va_start(ap, fmt);
    std::vsnprintf(&big[0], big.size(), fmt, ap);
    va_end(ap);
    out.append(big.data(), static_cast<std::size_t>(n));
}

}  // namespace

std::string emit_function_body_str(
    const Function& fn, const uint8_t* rom, std::size_t rom_size,
    uint32_t rom_base,
    const std::unordered_map<uint64_t, std::string>& func_names_by_key) {
    std::string out;

    const uint32_t step = (fn.mode == CpuMode::Thumb) ? 2u : 4u;
    armv4t::CodegenCtx ctx;
    ctx.names_by_key = &func_names_by_key;
    ctx.current_function_addr = fn.addr;
    ctx.current_function_end_addr = fn.end_addr;
    ctx.current_function_thumb = (fn.mode == CpuMode::Thumb);
    const uint32_t fn_source_addr = fn.source_addr ? fn.source_addr : fn.addr;

    auto source_offset_for = [&](uint32_t guest_pc, uint32_t len,
                                 std::size_t* out_off) -> bool {
        int64_t delta = static_cast<int64_t>(guest_pc) -
            static_cast<int64_t>(fn.addr);
        int64_t source_pc = static_cast<int64_t>(fn_source_addr) + delta;
        if (source_pc < static_cast<int64_t>(rom_base)) return false;
        uint64_t off64 = static_cast<uint64_t>(
            source_pc - static_cast<int64_t>(rom_base));
        if (off64 + len > rom_size) return false;
        *out_off = static_cast<std::size_t>(off64);
        return true;
    };

    auto plain_reg_source = [](const armv4t::Instr& ins, uint8_t* rm) {
        if (ins.op != armv4t::IrOp::MOV ||
            ins.op2.kind != armv4t::Op2::Kind::Shifted ||
            ins.op2.shifted.by_register ||
            ins.op2.shifted.type != armv4t::ShiftType::LSL ||
            ins.op2.shifted.imm_or_rs != 0) {
            return false;
        }
        *rm = ins.op2.shifted.rm;
        return true;
    };

    auto invalidate_written_aliases = [](const armv4t::Instr& ins,
                                         bool alias[16]) {
        auto clear = [&](uint8_t reg) {
            if (reg < 16) alias[reg] = false;
        };

        switch (ins.op) {
            case armv4t::IrOp::AND: case armv4t::IrOp::EOR:
            case armv4t::IrOp::SUB: case armv4t::IrOp::RSB:
            case armv4t::IrOp::ADD: case armv4t::IrOp::ADC:
            case armv4t::IrOp::SBC: case armv4t::IrOp::RSC:
            case armv4t::IrOp::ORR: case armv4t::IrOp::MOV:
            case armv4t::IrOp::BIC: case armv4t::IrOp::MVN:
            case armv4t::IrOp::LDR: case armv4t::IrOp::LDRB:
            case armv4t::IrOp::LDRH: case armv4t::IrOp::LDRSB:
            case armv4t::IrOp::LDRSH: case armv4t::IrOp::SWP:
            case armv4t::IrOp::SWPB: case armv4t::IrOp::MRS:
                clear(ins.rd);
                break;
            case armv4t::IrOp::MUL: case armv4t::IrOp::MLA:
                clear(ins.rd);
                break;
            case armv4t::IrOp::UMULL: case armv4t::IrOp::UMLAL:
            case armv4t::IrOp::SMULL: case armv4t::IrOp::SMLAL:
                clear(ins.rd);
                clear(ins.rn);
                break;
            case armv4t::IrOp::LDM:
                if (ins.block.load) {
                    for (uint8_t reg = 0; reg < 16; ++reg) {
                        if (ins.block.reg_list & (1u << reg)) {
                            clear(reg);
                        }
                    }
                }
                break;
            case armv4t::IrOp::BL:
            case armv4t::IrOp::BL_prefix:
            case armv4t::IrOp::BL_suffix:
                clear(14);
                break;
            default:
                break;
        }
    };

    // Stage 2 idle-loop elision: an instruction is "idle-simple" if it can
    // appear inside a quiescent (busy-wait) loop body without making the loop
    // unsafe to fast-forward — i.e. it has no architecturally-visible side
    // effect beyond writing a non-PC GP register / flags, and never transfers
    // control. Allowed: data-processing / MOV, single-register loads, the
    // multiplies, and MRS. Rejected (unsafe → ends an idle-candidate body):
    // any store (STR*/STM/SWP*), any call or branch (BL*/BX/BLX/B), LDM (may
    // load PC, multi-reg), SWI, MSR (mode/interrupt change), and any op that
    // writes PC (rd == 15). The single back-edge branch that closes the loop
    // is evaluated for eligibility BEFORE it is itself classified unsafe.
    auto is_idle_simple = [](const armv4t::Instr& ins) -> bool {
        switch (ins.op) {
            // Comparisons write only flags — always safe.
            case armv4t::IrOp::TST: case armv4t::IrOp::TEQ:
            case armv4t::IrOp::CMP: case armv4t::IrOp::CMN:
                return true;
            // GP-writing data processing / MOV / loads / MRS — safe unless
            // they write PC.
            case armv4t::IrOp::AND: case armv4t::IrOp::EOR:
            case armv4t::IrOp::SUB: case armv4t::IrOp::RSB:
            case armv4t::IrOp::ADD: case armv4t::IrOp::ADC:
            case armv4t::IrOp::SBC: case armv4t::IrOp::RSC:
            case armv4t::IrOp::ORR: case armv4t::IrOp::MOV:
            case armv4t::IrOp::BIC: case armv4t::IrOp::MVN:
            case armv4t::IrOp::LDR: case armv4t::IrOp::LDRB:
            case armv4t::IrOp::LDRH: case armv4t::IrOp::LDRSB:
            case armv4t::IrOp::LDRSH: case armv4t::IrOp::MRS:
                return ins.rd != 15;
            case armv4t::IrOp::MUL: case armv4t::IrOp::MLA:
                return ins.rd != 15;
            case armv4t::IrOp::UMULL: case armv4t::IrOp::UMLAL:
            case armv4t::IrOp::SMULL: case armv4t::IrOp::SMLAL:
                return ins.rd != 15 && ins.rn != 15;
            default:
                // Stores, SWP/SWPB, LDM, all branches, SWI, MSR, Undefined.
                return false;
        }
    };

    std::unordered_set<uint32_t> backward_targets;
    std::unordered_set<uint32_t> idle_backedge_pcs;
    // Highest PC of any non-idle-simple instruction seen so far in the forward
    // scan (-1 = none yet). A backward branch at P→T closes an idle-eligible
    // straight-line body iff no unsafe instruction lies in [T, P): last_unsafe
    // < T. Updated at the END of each scan step so it reflects only
    // instructions strictly before the current one.
    int64_t last_unsafe_pc = -1;
    std::unordered_set<uint32_t> bx_c_return_pcs;
    bool lr_alias[16] = {};
    lr_alias[14] = true;
    armv4t::Instr prev_scan_ins{};
    bool have_prev_scan_ins = false;

    // Function boundaries can split compact THUMB epilogues:
    //
    //   pop {r3}
    //   bx  r3
    //
    // If the finder discovered the BX as a separate entry, the scan below
    // starts at the BX and would otherwise miss the stack-pop return idiom.
    // Seed the previous instruction from ROM so the first decoded instruction
    // in this function can still be classified correctly.
    if (fn_source_addr >= rom_base && (fn_source_addr - rom_base) >= step) {
        const uint32_t prev_pc = fn.addr - step;
        std::size_t prev_off = 0;
        if (source_offset_for(prev_pc, step, &prev_off)) {
            if (fn.mode == CpuMode::Thumb) {
                uint16_t hw = static_cast<uint16_t>(
                    rom[prev_off] | (rom[prev_off + 1] << 8));
                prev_scan_ins = armv4t::ThumbDecoder::decode(hw, prev_pc);
            } else {
                uint32_t w = static_cast<uint32_t>(rom[prev_off])
                    | (static_cast<uint32_t>(rom[prev_off + 1]) << 8)
                    | (static_cast<uint32_t>(rom[prev_off + 2]) << 16)
                    | (static_cast<uint32_t>(rom[prev_off + 3]) << 24);
                prev_scan_ins = armv4t::ArmDecoder::decode(w, prev_pc);
            }
            have_prev_scan_ins = true;
        }
    }

    for (uint32_t scan_pc = fn.addr; scan_pc < fn.end_addr; scan_pc += step) {
        std::size_t scan_off = 0;
        if (!source_offset_for(scan_pc, step, &scan_off)) break;
        armv4t::Instr scan_ins;
        if (fn.mode == CpuMode::Thumb) {
            uint16_t hw = static_cast<uint16_t>(
                rom[scan_off] | (rom[scan_off + 1] << 8));
            scan_ins = armv4t::ThumbDecoder::decode(hw, scan_pc);
        } else {
            uint32_t w = static_cast<uint32_t>(rom[scan_off])
                | (static_cast<uint32_t>(rom[scan_off + 1]) << 8)
                | (static_cast<uint32_t>(rom[scan_off + 2]) << 16)
                | (static_cast<uint32_t>(rom[scan_off + 3]) << 24);
            scan_ins = armv4t::ArmDecoder::decode(w, scan_pc);
        }
        if (scan_ins.op == armv4t::IrOp::B &&
            scan_ins.branch_target >= fn.addr &&
            scan_ins.branch_target < fn.end_addr &&
            scan_ins.branch_target < scan_pc) {
            backward_targets.insert(scan_ins.branch_target);
            // Stage 2: this back-edge closes an idle-elision candidate iff the
            // straight-line body [target, scan_pc) is all idle-simple, i.e. no
            // unsafe instruction precedes it back to the target. (last_unsafe_pc
            // currently reflects instructions strictly before scan_pc, since it
            // is updated at the end of the step below.)
            if (last_unsafe_pc < static_cast<int64_t>(scan_ins.branch_target)) {
                idle_backedge_pcs.insert(scan_pc);
            }
        }
        if (scan_ins.op == armv4t::IrOp::BX &&
            have_prev_scan_ins &&
            prev_scan_ins.op == armv4t::IrOp::LDM &&
            prev_scan_ins.block.load &&
            prev_scan_ins.block.writeback &&
            prev_scan_ins.block.rn == 13 &&
            scan_ins.rm < 16 &&
            (prev_scan_ins.block.reg_list &
                static_cast<uint16_t>(1u << scan_ins.rm)) != 0) {
            bx_c_return_pcs.insert(scan_pc);
        }
        if (scan_ins.op == armv4t::IrOp::BX &&
            scan_ins.rm < 16 &&
            lr_alias[scan_ins.rm]) {
            bx_c_return_pcs.insert(scan_pc);
        }

        bool sets_lr_alias = false;
        uint8_t alias_dst = 0;
        uint8_t alias_src = 0;
        if (plain_reg_source(scan_ins, &alias_src) &&
            scan_ins.rd < 16 &&
            scan_ins.cond == armv4t::Cond::AL &&
            alias_src < 16 &&
            lr_alias[alias_src]) {
            sets_lr_alias = true;
            alias_dst = scan_ins.rd;
        }
        invalidate_written_aliases(scan_ins, lr_alias);
        if (sets_lr_alias) {
            lr_alias[alias_dst] = true;
        }

        // Stage 2: record the highest PC of any non-idle-simple instruction so
        // a later backward branch can test body cleanliness in O(1). Done after
        // the back-edge check above so the back-edge branch (itself unsafe) does
        // not disqualify the loop it closes.
        if (!is_idle_simple(scan_ins)) {
            last_unsafe_pc = static_cast<int64_t>(scan_pc);
        }

        prev_scan_ins = scan_ins;
        have_prev_scan_ins = true;
    }

    ctx.idle_backedge_pcs = &idle_backedge_pcs;

    uint32_t pc = fn.addr;
    while (pc < fn.end_addr) {
        std::size_t off = 0;
        if (!source_offset_for(pc, step, &off)) break;
        armv4t::Instr ins;
        if (fn.mode == CpuMode::Thumb) {
            uint16_t hw = static_cast<uint16_t>(
                rom[off] | (rom[off + 1] << 8));
            ins = armv4t::ThumbDecoder::decode(hw, pc);
        } else {
            uint32_t w = static_cast<uint32_t>(rom[off])
                | (static_cast<uint32_t>(rom[off + 1]) << 8)
                | (static_cast<uint32_t>(rom[off + 2]) << 16)
                | (static_cast<uint32_t>(rom[off + 3]) << 24);
            ins = armv4t::ArmDecoder::decode(w, pc);
        }

        if (backward_targets.count(pc) != 0) {
            appendf(out, "L_%08X:\n", pc);
        }

        std::string line = armv4t::format_ir(ins);
        appendf(out, "    /* %08X  %s */\n", pc, line.c_str());

        bool ni = false;
        ctx.force_bx_c_return = bx_c_return_pcs.count(pc) != 0;
        std::string emitted = armv4t::ArmCodegen::emit_instr(ins, ctx, &ni);
        out += emitted;
        // emit_instr already handles all return-shaped flows
        // (branches set PC + dispatch + return; SWI sets PC +
        // returns; LDM with PC in list returns; LDR PC dispatches).
        // For non-PC-writing instructions execution falls through
        // to the next emitted block.
        (void)ni;  // tracked for diagnostics; abort path is in the emit.
        pc += step;
    }
    // Fall-through tail-call dispatch.
    //
    // The function body may end for two reasons:
    //   1. A natural terminator (return, unconditional B, indirect
    //      branch, undefined). emit_instr() already emitted PC update
    //      + `return;`, so this trailing dispatch is dead code.
    //   2. The decode loop hit `fn.end_addr` (clipped to the next
    //      function's start by the finder). Execution is supposed
    //      to FALL THROUGH into the next function — typically when
    //      two callers enter a shared suffix at different points
    //      (e.g., the BIOS's reset_vector and SWI 0x26's handler
    //      share code starting at 0x8C).
    // In case 2, without this dispatch the runtime would return to
    // the exec loop with PC unchanged and re-dispatch the same
    // function forever. The trailing dispatch hands control to the
    // adjacent function via the dispatch table.
    appendf(out,
        "    /* fall-through to 0x%08X */\n"
        "    g_cpu.R[15] = 0x%08Xu;\n"
        "    runtime_dispatch(0x%08Xu);\n"
        "    return;\n",
        fn.end_addr, fn.end_addr, fn.end_addr);

    return out;
}

void emit_function_body(
    std::FILE* f, const Function& fn, const uint8_t* rom,
    std::size_t rom_size, uint32_t rom_base,
    const std::unordered_map<uint64_t, std::string>& func_names_by_key) {
    std::string body = emit_function_body_str(fn, rom, rom_size, rom_base,
                                              func_names_by_key);
    std::fwrite(body.data(), 1, body.size(), f);
}

}  // namespace gbarecomp
