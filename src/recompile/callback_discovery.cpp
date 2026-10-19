#include "function_finder.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include <cstdio>

namespace gbarecomp {
namespace {
uint64_t callback_key(uint32_t pc, CpuMode mode) {
    return (uint64_t(pc) << 1) | (mode == CpuMode::Thumb ? 1u : 0u);
}
}

bool FunctionFinder::is_thumb_callback_leaf(uint32_t entry) const {
    if ((entry & 1u) || entry < aot_scan_start_ ||
        uint64_t(entry)+2u > aot_scan_end_) return false;
    std::vector<uint32_t> pending{entry};
    std::unordered_set<uint32_t> seen;
    bool returned = false;
    while (!pending.empty()) {
        const uint32_t pc = pending.back(); pending.pop_back();
        if (!seen.insert(pc).second) continue;
        // This is a rejection budget, not an inferred function/table end.
        if (uint64_t(pc)+2u > uint64_t(entry)+64u ||
            uint64_t(pc)+2u > aot_scan_end_ || !can_read_at(pc, 2) ||
            addr_in_data_range(pc)) return false;
        const auto op = armv4t::ThumbDecoder::decode(read_u16(pc), pc);
        if (op.is_undefined || op.is_call || op.op == armv4t::IrOp::SWI)
            return false;
        if (op.op == armv4t::IrOp::BX && op.rm == 14) {
            returned = true;
            continue;
        }
        if (op.op == armv4t::IrOp::B) {
            if (op.branch_target <= pc) return false;
            pending.push_back(op.branch_target);
            if (op.cond != armv4t::Cond::AL) pending.push_back(pc+2u);
            continue;
        }
        if (op.is_branch) return false;
        // A frameless callback may read its arguments, manipulate memory and
        // multiply state. It must preserve SP, LR and all callee-saved regs.
        // Explicitly whitelist ordinary integer/data operations; stack/block
        // transfers and unknown operations cannot qualify data as a leaf.
        switch (op.op) {
            case armv4t::IrOp::MOV: case armv4t::IrOp::MVN:
            case armv4t::IrOp::ADD: case armv4t::IrOp::SUB:
            case armv4t::IrOp::RSB: case armv4t::IrOp::ADC:
            case armv4t::IrOp::SBC: case armv4t::IrOp::AND:
            case armv4t::IrOp::EOR: case armv4t::IrOp::ORR:
            case armv4t::IrOp::BIC: case armv4t::IrOp::MUL:
            case armv4t::IrOp::CMP: case armv4t::IrOp::CMN:
            case armv4t::IrOp::TST: case armv4t::IrOp::TEQ:
                if (op.rd >= 4 && op.rd != 12 &&
                    op.op != armv4t::IrOp::CMP && op.op != armv4t::IrOp::CMN &&
                    op.op != armv4t::IrOp::TST && op.op != armv4t::IrOp::TEQ)
                    return false;
                break;
            case armv4t::IrOp::LDR: case armv4t::IrOp::LDRB:
            case armv4t::IrOp::LDRH: case armv4t::IrOp::LDRSB:
            case armv4t::IrOp::LDRSH:
                if ((op.rd >= 4 && op.rd != 12) || op.mem.writeback ||
                    op.mem.rn == 13) return false;
                break;
            case armv4t::IrOp::STR: case armv4t::IrOp::STRB:
            case armv4t::IrOp::STRH:
                if (op.mem.writeback || op.mem.rn == 13) return false;
                break;
            default: return false;
        }
        pending.push_back(pc+2u);
    }
    return returned;
}

void FunctionFinder::harvest_stored_callback(uint32_t raw) {
    const uint32_t target = raw & ~1u;
    if (aot_scan_start_ >= aot_scan_end_ || !(raw & 1u) ||
        target < aot_scan_start_ || target >= aot_scan_end_ ||
        !addr_in_rom(target) || addr_in_data_range(target) ||
        aot_seen_seed_keys_.count(callback_key(target, CpuMode::Thumb))) return;
    const bool frame = uint64_t(target)+12u <= aot_scan_end_ &&
        is_thumb_func_prologue(target) && looks_like_code(target, CpuMode::Thumb, true);
    if (!frame && !is_thumb_callback_leaf(target)) return;
    aot_seen_seed_keys_.insert(callback_key(target, CpuMode::Thumb));
    aot_pointer_seeds_.push_back({target, CpuMode::Thumb, "", 0, true});
    ++stats_.aot_callback_seeds;
}

void FunctionFinder::harvest_contextual_callbacks(
        uint32_t entry, uint8_t known_mask,
        const std::array<uint32_t, 4>& arguments) {
    if (aot_scan_start_ >= aot_scan_end_ || !known_mask || (entry & 1u) ||
        !addr_in_rom(entry) || !can_read_at(entry, 2) || addr_in_data_range(entry)) return;
    std::array<uint32_t, 6> key{entry, known_mask, 0, 0, 0, 0};
    for (unsigned r = 0; r < 4; ++r)
        if (known_mask & (1u << r)) key[2+r] = arguments[r];
    if (callback_contexts_.size() >= 262144u) {
        if (!callback_context_budget_reported_) {
            std::fprintf(stderr, "[finder] callback call-site analysis budget exhausted; additional contexts remain unexamined\n");
            callback_context_budget_reported_ = true;
        }
        return;
    }
    if (!callback_contexts_.insert(key).second) return;
    struct Value { bool known = false; uint32_t value = 0, origin = 0; };
    struct State {
        uint32_t pc;
        std::array<Value, 16> regs{};
        bool flags = false, n = false, z = false, c = false, v = false;
    };
    State initial{entry};
    for (unsigned r = 0; r < 4; ++r)
        if (known_mask & (1u << r)) initial.regs[r] = {true, arguments[r], 0};
    std::vector<State> pending{initial};
    unsigned budget = 256;
    while (!pending.empty() && budget) {
        State state = pending.back(); pending.pop_back();
        while (budget) {
            --budget;
            const uint32_t pc = state.pc;
            if (pc < entry || uint64_t(pc)+2u > uint64_t(entry)+128u ||
                !can_read_at(pc, 2) || addr_in_data_range(pc)) break;
            const auto op = armv4t::ThumbDecoder::decode(read_u16(pc), pc);
            if (op.is_undefined || op.op == armv4t::IrOp::SWI) break;
            auto reg = [&](uint8_t r, uint32_t& value) {
                if (r >= 15 || !state.regs[r].known) return false;
                value = state.regs[r].value; return true;
            };
            auto operand = [&](uint32_t& value) {
                if (op.op2.kind == armv4t::Op2::Kind::Imm) {
                    value = op.op2.imm_value; return true;
                }
                if (op.op2.kind != armv4t::Op2::Kind::Shifted ||
                    op.op2.shifted.by_register || !reg(op.op2.shifted.rm, value)) return false;
                const unsigned shift = op.op2.shifted.imm_or_rs;
                if (shift > 32u) return false;
                switch (op.op2.shifted.type) {
                    case armv4t::ShiftType::LSL: value = shift == 32 ? 0 : value << shift; return true;
                    case armv4t::ShiftType::LSR: value = !shift || shift == 32 ? 0 : value >> shift; return true;
                    case armv4t::ShiftType::ASR:
                        if (!shift || shift == 32) value = (value & 0x80000000u) ? ~0u : 0u;
                        else value = (value >> shift) | ((value & 0x80000000u) ? (~0u << (32u-shift)) : 0u);
                        return true;
                    default: return false;
                }
            };
            auto branch_taken = [&]() -> int {
                if (op.cond == armv4t::Cond::AL) return 1;
                if (!state.flags) return -1;
                switch (op.cond) {
                    case armv4t::Cond::EQ: return state.z;
                    case armv4t::Cond::NE: return !state.z;
                    case armv4t::Cond::CS: return state.c;
                    case armv4t::Cond::CC: return !state.c;
                    case armv4t::Cond::MI: return state.n;
                    case armv4t::Cond::PL: return !state.n;
                    case armv4t::Cond::VS: return state.v;
                    case armv4t::Cond::VC: return !state.v;
                    case armv4t::Cond::HI: return state.c && !state.z;
                    case armv4t::Cond::LS: return !state.c || state.z;
                    case armv4t::Cond::GE: return state.n == state.v;
                    case armv4t::Cond::LT: return state.n != state.v;
                    case armv4t::Cond::GT: return !state.z && state.n == state.v;
                    case armv4t::Cond::LE: return state.z || state.n != state.v;
                    default: return -1;
                }
            };
            if (op.op == armv4t::IrOp::B) {
                const int taken = branch_taken();
                if (taken != 0 && op.branch_target > pc && pending.size() < 16) {
                    State next = state; next.pc = op.branch_target;
                    pending.push_back(next);
                }
                if (taken == 1) break;
                state.pc += 2u;
                continue;
            }
            if (op.op == armv4t::IrOp::BL_prefix && can_read_at(pc+2u, 2)) {
                const auto low = armv4t::ThumbDecoder::decode(read_u16(pc+2u), pc+2u);
                if (low.op != armv4t::IrOp::BL_suffix) break;
                const uint32_t callee = (op.branch_target + low.swi_imm) & ~1u;
                if (!can_read_at(callee, 2) || addr_in_data_range(callee)) break;
                const auto veneer = armv4t::ThumbDecoder::decode(read_u16(callee), callee);
                if (veneer.op == armv4t::IrOp::BX && veneer.rm != 14 &&
                    veneer.rm < 15 && state.regs[veneer.rm].known) {
                    const auto pointer = state.regs[veneer.rm];
                    const uint32_t target = pointer.value & ~1u;
                    const CpuMode mode = (pointer.value & 1u) ? CpuMode::Thumb : CpuMode::Arm;
                    if (target >= aot_scan_start_ && target < aot_scan_end_ &&
                        addr_in_rom(target) && looks_like_code(target, mode, false)) {
                        if (aot_seen_seed_keys_.insert(callback_key(target, mode)).second) {
                            aot_pointer_seeds_.push_back({target, mode, "", 0, true});
                            ++stats_.aot_callback_seeds;
                        }
                        // One proved called field can corroborate the preceding
                        // tagged callback field, including a return-proved leaf.
                        if (pointer.origin >= rom_base_+4u && can_read_at(pointer.origin-4u, 4))
                            harvest_stored_callback(read_u32(pointer.origin-4u));
                    }
                }
                // Never assume a callee preserves a register or execute a SWI.
                // This pass only finds pointers before the first opaque call.
                break;
            }
            if (op.is_branch || op.is_call || op.cond != armv4t::Cond::AL) break;
            if (op.set_flags) state.flags = false;
            uint32_t left = 0, right = 0;
            if (op.op == armv4t::IrOp::CMP) {
                if (reg(op.rn, left) && operand(right)) {
                    const uint32_t result = left-right;
                    state.flags = true; state.n = (result >> 31) != 0;
                    state.z = result == 0; state.c = left >= right;
                    state.v = (((left ^ right) & (left ^ result)) >> 31) != 0;
                }
            } else if (op.op == armv4t::IrOp::MOV) {
                const bool known = operand(right);
                const bool copy = known && op.op2.kind == armv4t::Op2::Kind::Shifted &&
                    op.op2.shifted.type == armv4t::ShiftType::LSL && op.op2.shifted.imm_or_rs == 0;
                const uint32_t origin = copy ? state.regs[op.op2.shifted.rm].origin : 0u;
                if (op.rd < 15) state.regs[op.rd] = {known, right, origin};
            } else if (op.op == armv4t::IrOp::ADD || op.op == armv4t::IrOp::SUB ||
                       op.op == armv4t::IrOp::AND || op.op == armv4t::IrOp::ORR ||
                       op.op == armv4t::IrOp::EOR || op.op == armv4t::IrOp::BIC ||
                       op.op == armv4t::IrOp::MUL) {
                const bool pc_base = op.rn == 15 && op.op2.kind == armv4t::Op2::Kind::Imm;
                const bool have_left = pc_base ? (left = (pc+4u)&~3u, true) : reg(op.rn, left);
                const bool known = have_left && operand(right);
                uint32_t value = 0;
                if (known) switch (op.op) {
                    case armv4t::IrOp::ADD: value = left+right; break;
                    case armv4t::IrOp::SUB: value = left-right; break;
                    case armv4t::IrOp::AND: value = left&right; break;
                    case armv4t::IrOp::ORR: value = left|right; break;
                    case armv4t::IrOp::EOR: value = left^right; break;
                    case armv4t::IrOp::BIC: value = left&~right; break;
                    case armv4t::IrOp::MUL: value = left*right; break;
                    default: break;
                }
                if (op.rd < 15) state.regs[op.rd] = {known, value, 0};
            } else if (op.op == armv4t::IrOp::LDR || op.op == armv4t::IrOp::LDRB ||
                       op.op == armv4t::IrOp::LDRH || op.op == armv4t::IrOp::LDRSB ||
                       op.op == armv4t::IrOp::LDRSH) {
                bool known = !op.mem.writeback && op.mem.pre_indexed;
                if (op.mem.rn == 15) left = (pc+4u)&~3u;
                else known = known && reg(op.mem.rn, left);
                right = op.mem.imm_offset;
                if (op.mem.by_register) {
                    const auto& shift = op.mem.reg_offset;
                    known = known && !shift.by_register && shift.type == armv4t::ShiftType::LSL &&
                        shift.imm_or_rs < 32u && reg(shift.rm, right);
                    if (known) right <<= shift.imm_or_rs;
                }
                const uint32_t address = op.mem.add ? left+right : left-right;
                const unsigned width = op.op == armv4t::IrOp::LDR ? 4u :
                    (op.op == armv4t::IrOp::LDRH || op.op == armv4t::IrOp::LDRSH) ? 2u : 1u;
                known = known && !(address & (width-1u)) && addr_in_rom(address) && can_read_at(address, width);
                uint32_t value = 0;
                if (known) {
                    if (width == 4) value = read_u32(address);
                    else if (width == 2) value = read_u16(address);
                    else {
                        uint32_t source = 0;
                        known = map_addr_to_source(address, &source);
                        if (known) value = rom_[source-rom_base_];
                    }
                    if (op.op == armv4t::IrOp::LDRSB && (value & 0x80u)) value |= 0xFFFFFF00u;
                    if (op.op == armv4t::IrOp::LDRSH && (value & 0x8000u)) value |= 0xFFFF0000u;
                }
                if (op.rd < 15) state.regs[op.rd] = {known, value, known && width == 4 ? address : 0u};
            } else if (op.op == armv4t::IrOp::STR) {
                if (!op.mem.writeback && op.rd < 15 && state.regs[op.rd].known)
                    harvest_stored_callback(state.regs[op.rd].value);
            } else if (op.op == armv4t::IrOp::STRB || op.op == armv4t::IrOp::STRH ||
                       op.op == armv4t::IrOp::CMN || op.op == armv4t::IrOp::TST ||
                       op.op == armv4t::IrOp::TEQ) {
                // No register is written by these instructions.
            } else if (op.op == armv4t::IrOp::LDM || op.op == armv4t::IrOp::STM) {
                if (op.block.load) for (unsigned r = 0; r < 16; ++r)
                    if (op.block.reg_list & (1u << r)) state.regs[r] = {};
                if (op.block.writeback && op.block.rn < 16) state.regs[op.block.rn] = {};
            } else {
                if (op.rd < 16) state.regs[op.rd] = {};
            }
            state.pc += 2u;
        }
    }
}

} // namespace gbarecomp
