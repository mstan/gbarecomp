#include "function_finder.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include <cstdio>
#include <map>
#include <bit>

namespace gbarecomp {
namespace {
uint64_t callback_key(uint32_t pc, CpuMode mode) {
    return (uint64_t(pc) << 1) | (mode == CpuMode::Thumb ? 1u : 0u);
}
}

FunctionFinder::ThumbCallEffects FunctionFinder::thumb_call_effects(uint32_t entry, CpuMode mode, unsigned depth) {
    const uint64_t effect_key=callback_key(entry,mode);
    if (const auto found = thumb_call_effects_.find(effect_key); found != thumb_call_effects_.end())
        return found->second;
    if (depth >= 12u || (entry & (mode==CpuMode::Arm ? 3u : 1u)) || !addr_in_rom(entry) ||
        !can_read_at(entry, 2) || addr_in_data_range(entry) ||
        !thumb_call_effects_active_.insert(effect_key).second) return {};
    // Origins represent incoming registers, not their runtime values. Stack
    // cells retain those origins across PUSH/POP. No ABI preservation is
    // assumed for direct callees: every reachable return must prove it.
    constexpr int32_t unknown = -1, stack_base = 65536;
    auto stack_value = [](int32_t value) { return value >= 61440 && value <= 69632; };
    struct State {
        uint32_t pc;
        CpuMode mode=CpuMode::Thumb;
        std::array<int32_t,16> regs{};
        std::map<int32_t,int32_t> stack;
    };
    State initial{};initial.pc=entry;initial.mode=mode;
    for (unsigned r=0;r<16;++r) initial.regs[r]=int32_t(r);
    initial.regs[13]=stack_base;
    std::vector<State> pending{initial};
    std::set<std::vector<int32_t>> seen;
    ThumbCallEffects result{true,0xFFFFu,false};
    bool returned=false;
    unsigned budget=2048;
    auto finish = [&](const State& state, int32_t target) {
        if (target!=14 || state.regs[13]!=stack_base) return false;
        uint16_t preserved=0;
        for (unsigned r=0;r<15;++r)
            if (state.regs[r]==(r==13 ? stack_base : int32_t(r))) preserved|=uint16_t(1u<<r);
        result.preserved &= preserved;returned=true;return true;
    };
    while (!pending.empty() && result.valid && budget) {
        State state=std::move(pending.back());pending.pop_back();
        while (result.valid && budget) {
            --budget;
            const uint32_t pc=state.pc;
            const uint32_t step=state.mode==CpuMode::Thumb ? 2u : 4u;
            if (pc<entry || uint64_t(pc)+step>uint64_t(entry)+4096u ||
                !can_read_at(pc,step) || addr_in_data_range(pc) || state.stack.size()>128u) {
                result.valid=false;break;
            }
            std::vector<int32_t> key{int32_t(pc),int32_t(step)};
            key.insert(key.end(),state.regs.begin(),state.regs.end());
            for (const auto& cell:state.stack) { key.push_back(cell.first);key.push_back(cell.second); }
            if (!seen.insert(std::move(key)).second) break;
            const auto op=state.mode==CpuMode::Thumb
                ? armv4t::ThumbDecoder::decode(read_u16(pc),pc)
                : armv4t::ArmDecoder::decode(read_u32(pc),pc);
            if (op.is_undefined || op.op==armv4t::IrOp::SWI) {result.valid=false;break;}
            if (op.op==armv4t::IrOp::B) {
                State branch=state;branch.pc=op.branch_target;pending.push_back(std::move(branch));
                if (op.cond==armv4t::Cond::AL) break;
                state.pc+=step;continue;
            }
            if (state.mode==CpuMode::Arm && op.cond!=armv4t::Cond::AL) {
                State skipped=state;skipped.pc+=step;pending.push_back(std::move(skipped));
            }
            if (op.op==armv4t::IrOp::BL || op.op==armv4t::IrOp::BL_prefix) {
                uint32_t callee=op.branch_target,call_size=step;
                if(op.op==armv4t::IrOp::BL_prefix) {
                    if(!can_read_at(pc+2u,2)) {result.valid=false;break;}
                    const auto low=armv4t::ThumbDecoder::decode(read_u16(pc+2u),pc+2u);
                    if (low.op!=armv4t::IrOp::BL_suffix) {result.valid=false;break;}
                    callee=(op.branch_target+low.swi_imm)&~1u;call_size=4;
                }
                const auto effects=thumb_call_effects(callee,state.mode,depth+1u);
                if (!effects.valid) {result.valid=false;break;}
                for (unsigned r=0;r<15;++r)
                    if (!(effects.preserved & (1u<<r))) state.regs[r]=unknown;
                state.regs[14]=unknown; // BL itself replaces LR even for a leaf
                if (effects.writes_caller_stack) {
                    for (auto& cell:state.stack) if (cell.first>=state.regs[13]-stack_base)
                        cell.second=unknown;
                }
                result.writes_caller_stack |= effects.writes_caller_stack;
                state.pc+=call_size;continue;
            }
            if (op.op==armv4t::IrOp::BX) {
                // GCC's memcpy helpers enter ARM with Thumb BX PC. Follow the
                // actual instruction-state change rather than assuming ABI
                // effects for a library name or treating ARM words as Thumb.
                if(state.mode==CpuMode::Thumb && op.rm==15) {
                    state.pc=(pc+4u)&~3u;state.mode=CpuMode::Arm;continue;
                }
                result.valid=op.rm<16 && finish(state,state.regs[op.rm]);break;
            }
            const auto& operand=op.op2;
            if (op.op==armv4t::IrOp::STM || op.op==armv4t::IrOp::LDM) {
                if(op.block.rn>=15 || !op.block.reg_list ||
                    (op.block.writeback && (op.block.reg_list & (1u<<op.block.rn)))) {result.valid=false;break;}
                if(!stack_value(state.regs[op.block.rn])) {
                    if(op.block.load && (op.block.reg_list & 0x8000u)) {result.valid=false;break;}
                    if(op.block.load) for(unsigned r=0;r<16;++r)
                        if(op.block.reg_list & (1u<<r)) state.regs[r]=unknown;
                    if(op.block.writeback) state.regs[op.block.rn]=unknown;
                    state.pc+=step;continue;
                }
                unsigned n=0;for(unsigned r=0;r<16;++r) n+=(op.block.reg_list>>r)&1u;
                const int32_t sp=state.regs[op.block.rn]-stack_base;
                int32_t at=sp+(op.block.add ? (op.block.pre_indexed ? 4 : 0)
                    : (op.block.pre_indexed ? -4*int32_t(n) : -4*int32_t(n-1u)));
                for (unsigned r=0;r<16;++r) if (op.block.reg_list & (1u<<r)) {
                    if (op.block.load) {
                        const auto cell=state.stack.find(at);
                        state.regs[r]=cell==state.stack.end() ? unknown : cell->second;
                    } else {state.stack[at]=state.regs[r];if(at>=0) result.writes_caller_stack=true;}
                    at+=4;
                }
                if(op.block.writeback) state.regs[op.block.rn]+=op.block.add ? 4*int32_t(n) : -4*int32_t(n);
                if (op.block.load && (op.block.reg_list & 0x8000u)) {
                    result.valid=finish(state,state.regs[15]);break;
                }
            } else if (op.op==armv4t::IrOp::MOV || op.op==armv4t::IrOp::ADD || op.op==armv4t::IrOp::SUB) {
                int32_t value=unknown;
                if (op.op==armv4t::IrOp::MOV && operand.kind==armv4t::Op2::Kind::Shifted &&
                    !operand.shifted.by_register && operand.shifted.type==armv4t::ShiftType::LSL &&
                    operand.shifted.imm_or_rs==0 && operand.shifted.rm<16)
                    value=state.regs[operand.shifted.rm];
                if (op.op!=armv4t::IrOp::MOV && op.rn<16 && stack_value(state.regs[op.rn]) &&
                    operand.kind==armv4t::Op2::Kind::Imm) {
                    const int64_t adjusted=int64_t(state.regs[op.rn])+
                        (op.op==armv4t::IrOp::ADD ? int64_t(operand.imm_value) : -int64_t(operand.imm_value));
                    if (adjusted>=61440 && adjusted<=69632) value=int32_t(adjusted);
                }
                if (op.rd==15) {result.valid=finish(state,value);break;}
                if (op.rd<16) state.regs[op.rd]=value;
            } else if (op.op==armv4t::IrOp::LDR || op.op==armv4t::IrOp::LDRB ||
                       op.op==armv4t::IrOp::LDRH || op.op==armv4t::IrOp::LDRSB ||
                       op.op==armv4t::IrOp::LDRSH || op.op==armv4t::IrOp::STR ||
                       op.op==armv4t::IrOp::STRB || op.op==armv4t::IrOp::STRH) {
                const bool store=op.op==armv4t::IrOp::STR || op.op==armv4t::IrOp::STRB || op.op==armv4t::IrOp::STRH;
                const unsigned width=(op.op==armv4t::IrOp::LDR || op.op==armv4t::IrOp::STR) ? 4u :
                    (op.op==armv4t::IrOp::LDRH || op.op==armv4t::IrOp::LDRSH || op.op==armv4t::IrOp::STRH) ? 2u : 1u;
                int32_t value=unknown;
                if (op.mem.rn<16 && stack_value(state.regs[op.mem.rn])) {
                    if (op.mem.by_register || op.mem.writeback || !op.mem.pre_indexed) {result.valid=false;break;}
                    const int32_t at=state.regs[op.mem.rn]-stack_base+
                        (op.mem.add ? int32_t(op.mem.imm_offset) : -int32_t(op.mem.imm_offset));
                    if (store) {
                        for(auto& cell:state.stack) if(cell.first<at+int32_t(width) && cell.first+4>at) cell.second=unknown;
                        if(width==4 && !(at&3)) state.stack[at]=op.rd<16 ? state.regs[op.rd] : unknown;
                        if(at>=0) result.writes_caller_stack=true;
                    } else if(width==4 && !(at&3)) {
                        const auto cell=state.stack.find(at);if(cell!=state.stack.end()) value=cell->second;
                    }
                }
                if(!store && op.rd<16) state.regs[op.rd]=value;
                if(op.mem.writeback && op.mem.rn<16) state.regs[op.mem.rn]=unknown;
            } else if (op.op!=armv4t::IrOp::CMP && op.op!=armv4t::IrOp::CMN &&
                       op.op!=armv4t::IrOp::TST && op.op!=armv4t::IrOp::TEQ) {
                if(op.is_branch || op.is_call) {result.valid=false;break;}
                if(op.rd<16) state.regs[op.rd]=unknown;
            }
            state.pc+=step;
        }
    }
    result.valid=result.valid && returned && pending.empty() && budget>0;
    if(!result.valid) result={};
    thumb_call_effects_active_.erase(effect_key);
    thumb_call_effects_[effect_key]=result;
    return result;
}

std::vector<uint32_t> FunctionFinder::thumb_ram_store_roots(uint32_t entry) const {
    struct Value { bool known=false; uint32_t value=0, root=0; };
    std::array<Value,16> regs{};
    std::set<uint32_t> roots;
    if ((entry & 1u) || entry<aot_scan_start_ || entry>=aot_scan_end_) return {};
    // Only exact destinations in the initial basic block contribute. A literal
    // used for a read, or a pointer loaded from mutable RAM, is not a store root.
    // This fixed rejection budget never declares a function or table extent.
    for(uint32_t pc=entry;uint64_t(pc)+2u<=uint64_t(entry)+64u;pc+=2u) {
        if(uint64_t(pc)+2u>aot_scan_end_ || !can_read_at(pc,2) || addr_in_data_range(pc)) break;
        const auto op=armv4t::ThumbDecoder::decode(read_u16(pc),pc);
        if(op.is_undefined || op.is_branch || op.is_call || op.op==armv4t::IrOp::SWI) break;
        auto operand = [&]() -> Value {
            if(op.op2.kind==armv4t::Op2::Kind::Imm) return {true,op.op2.imm_value,0};
            if(op.op2.kind==armv4t::Op2::Kind::Shifted && !op.op2.shifted.by_register &&
               op.op2.shifted.type==armv4t::ShiftType::LSL && op.op2.shifted.imm_or_rs==0 &&
               op.op2.shifted.rm<15) return regs[op.op2.shifted.rm];
            return {};
        };
        if(op.op==armv4t::IrOp::STM && op.block.rn==13 && op.block.pre_indexed &&
           !op.block.add && op.block.writeback) { regs[13]={};continue; }
        if(op.op==armv4t::IrOp::LDM || op.op==armv4t::IrOp::STM) break;
        if(op.op==armv4t::IrOp::STR || op.op==armv4t::IrOp::STRB || op.op==armv4t::IrOp::STRH) {
            if(op.mem.writeback || !op.mem.pre_indexed || op.mem.rn>=15) break;
            const auto base=regs[op.mem.rn];
            bool known=base.known;
            uint32_t offset=op.mem.imm_offset;
            if(op.mem.by_register) {
                const auto rm=op.mem.reg_offset.rm;
                known=known && rm<15 && regs[rm].known &&
                    !op.mem.reg_offset.by_register && op.mem.reg_offset.type==armv4t::ShiftType::LSL &&
                    op.mem.reg_offset.imm_or_rs==0;
                if(known) offset=regs[rm].value;
            }
            const int64_t address=int64_t(base.value)+(op.mem.add ? int64_t(offset) : -int64_t(offset));
            const uint32_t width=op.op==armv4t::IrOp::STR ? 4u : op.op==armv4t::IrOp::STRH ? 2u : 1u;
            if(known && base.root && address>=0x02000000 && address+width<=0x04000000 &&
               (address & (width-1u))==0) roots.insert(base.root);
            continue;
        }
        if(op.op==armv4t::IrOp::CMP || op.op==armv4t::IrOp::CMN ||
           op.op==armv4t::IrOp::TST || op.op==armv4t::IrOp::TEQ) continue;
        Value value{};
        if(op.op==armv4t::IrOp::LDR && op.mem.rn==15 && !op.mem.by_register && !op.mem.writeback) {
            const uint32_t base=(pc+4u)&~3u;
            const uint32_t literal=op.mem.add ? base+op.mem.imm_offset : base-op.mem.imm_offset;
            if(can_read_at(literal,4)) {
                value={true,read_u32(literal),0};
                if(value.value>=0x02000000u && value.value<0x04000000u) value.root=value.value;
            }
        } else if(op.op==armv4t::IrOp::MOV) value=operand();
        else if((op.op==armv4t::IrOp::ADD || op.op==armv4t::IrOp::SUB) && op.rn<15) {
            const auto left=regs[op.rn],right=operand();
            if(left.known && right.known && !right.root) {
                value={true,op.op==armv4t::IrOp::ADD ? left.value+right.value : left.value-right.value,left.root};
                if(value.value<0x02000000u || value.value>=0x04000000u) value.root=0;
            }
        }
        if(op.rd>=15) break;
        regs[op.rd]=value; // other loads/arithmetic retire the old destination
    }
    return {roots.begin(),roots.end()};
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
        const std::array<uint32_t, 4>& arguments, unsigned depth, bool local_branch,
        CallbackFlow flow) {
    if (depth >= 16u || aot_scan_start_ >= aot_scan_end_ || (!known_mask && !local_branch) || (entry & 1u) ||
        !addr_in_rom(entry) || !can_read_at(entry, 2) || addr_in_data_range(entry)) return;
    std::array<uint32_t, 6> key{entry, known_mask, 0, 0, 0, 0};
    for (unsigned r = 0; r < 4; ++r)
        if (known_mask & (1u << r)) key[2+r] = arguments[r];
    if(local_branch) {
        if(callback_branch_contexts_.size()>=4096u) {
            if(!callback_branch_budget_reported_) {
                std::fprintf(stderr,"[finder] callback branch analysis budget exhausted; additional branch contexts remain unexamined\n");
                callback_branch_budget_reported_=true;
            }
            return;
        }
        if(!callback_branch_contexts_.insert(entry).second) return;
    }
    if (local_branch) flow = CallbackFlow::Branch;
    // Enumerating many byte values for one helper must not starve a later
    // directly known callback argument. Recursive calls retain their original
    // producer's quota; branch-derived work cannot spend either older quota.
    const bool branch_flow = flow == CallbackFlow::Branch;
    const bool byte_flow = flow == CallbackFlow::ByteDomain;
    auto& contexts = branch_flow ? callback_branch_call_contexts_ : byte_flow ? callback_byte_contexts_ : callback_contexts_;
    auto& reported = branch_flow ? callback_branch_call_budget_reported_ : byte_flow ? callback_byte_budget_reported_ : callback_context_budget_reported_;
    const std::size_t context_limit = branch_flow ? 65536u : 262144u;
    if (!local_branch && contexts.size() >= context_limit) {
        if (!reported) {
            std::fprintf(stderr, "[finder] callback %s analysis budget exhausted; additional contexts remain unexamined\n",
                         branch_flow ? "branch call-site" : byte_flow ? "byte-domain call-site" : "call-site");
            reported = true;
        }
        return;
    }
    if (!local_branch && !contexts.insert(key).second) return;
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
    unsigned budget = 8192;
    while (!pending.empty() && budget) {
        State state = pending.back(); pending.pop_back();
        while (budget) {
            --budget;
            const uint32_t pc = state.pc;
            if (pc < entry || uint64_t(pc)+2u > uint64_t(entry)+2048u ||
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
                if (taken != 0 && pending.size() < 16) {
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
                    break; // the indirect callee's effects remain unknown
                }
                uint8_t nested_mask=0;
                std::array<uint32_t,4> nested_arguments{};
                for(unsigned r=0;r<4;++r) if(state.regs[r].known) {
                    nested_mask|=uint8_t(1u<<r);nested_arguments[r]=state.regs[r].value;
                }
                harvest_contextual_callbacks(callee,nested_mask,nested_arguments,depth+1u,false,flow);
                const auto effects=thumb_call_effects(callee);
                if(!effects.valid) break;
                for(unsigned r=0;r<15;++r)
                    if(!(effects.preserved & (1u<<r))) state.regs[r]={};
                state.regs[14]={};state.flags=false;
                state.pc+=4u;continue;
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
            } else if(op.op==armv4t::IrOp::MUL) {
                // MUL has dedicated Rm/Rs operands. Its unused Op2 defaults
                // cannot prove a zero product when the runtime input is unknown.
                const bool known=reg(op.rm,left) && reg(op.rs,right);
                if(op.rd<15)state.regs[op.rd]={known,known ? left*right : 0u,0};
            } else if (op.op == armv4t::IrOp::ADD || op.op == armv4t::IrOp::SUB ||
                       op.op == armv4t::IrOp::AND || op.op == armv4t::IrOp::ORR ||
                       op.op == armv4t::IrOp::EOR || op.op == armv4t::IrOp::BIC) {
                const bool pc_base = op.rn == 15 && op.op2.kind == armv4t::Op2::Kind::Imm;
                const bool have_left = pc_base ? (left = (pc+4u)&~3u, true) : reg(op.rn, left);
                const bool have_right=operand(right);
                if(op.op==armv4t::IrOp::AND && op.rd<15 && have_left!=have_right) {
                    const uint32_t mask=have_left ? left : right;
                    if(mask<=255u && std::popcount(mask)<=4) {
                        // Enumerate the exact finite image of x & mask. Values
                        // with bits outside mask are impossible, even when
                        // they sit between zero and mask in the ROM table.
                        for(uint32_t value=mask;value;value=(value-1u)&mask) {
                            State choice=state;choice.pc+=2u;choice.flags=false;
                            choice.regs[op.rd]={true,value,0};pending.push_back(choice);
                        }
                        state.regs[op.rd]={true,0,0};state.flags=false;state.pc+=2u;
                        continue;
                    }
                }
                const bool known = have_left && have_right;
                uint32_t value = 0;
                if (known) switch (op.op) {
                    case armv4t::IrOp::ADD: value = left+right; break;
                    case armv4t::IrOp::SUB: value = left-right; break;
                    case armv4t::IrOp::AND: value = left&right; break;
                    case armv4t::IrOp::ORR: value = left|right; break;
                    case armv4t::IrOp::EOR: value = left^right; break;
                    case armv4t::IrOp::BIC: value = left&~right; break;
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
