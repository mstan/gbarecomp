// function_finder.cpp — implementation.
//
// Discovery is a prioritized worklist:
//   - Pop (addr, mode) from the worklist.
//   - If already visited, skip.
//   - Decode instructions from addr forward, in `mode`.
//   - For each direct branch (B / BL / conditional B), record the
//     target and add it to the worklist as a NEW function root near
//     the current function (the
//     finder is intentionally pessimistic — every branch target is
//     treated as a potential function entry; the codegen later
//     consolidates).
//   - For BX/BLX-reg/computed: mark `has_indirect_transfer` and stop
//     this function's body.
//   - Stop at: undefined, return-shaped instr (BX LR / pop {pc} /
//     ldm rN, {..., pc}), or after a hard cap (4096 instrs per
//     function as a sanity bound).

#include "function_finder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <deque>

#include "arm_decode.h"
#include "thumb_decode.h"
#include "arm_ir.h"

namespace gbarecomp {

namespace {

uint64_t visit_key(uint32_t addr, CpuMode mode) {
    return (static_cast<uint64_t>(addr) << 1) |
           static_cast<uint64_t>(mode);
}

std::string default_name(uint32_t addr, CpuMode mode) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s_%08X",
                  mode == CpuMode::Thumb ? "tfunc" : "afunc",
                  addr);
    return std::string(buf);
}

uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31u;
    if (n == 0) return v;
    return (v >> n) | (v << (32u - n));
}

// Rd is also a source operand for stores/compares and is unused (zero) in
// block transfers. Invalidate a loaded callback only on an actual write;
// unknown calls may clobber any register regardless of their encoded Rd.
bool may_clobber_register(const armv4t::Instr& ins, uint8_t reg) {
    using armv4t::IrOp;
    if (ins.is_call || ins.op == IrOp::SWI) return true;
    switch (ins.op) {
        case IrOp::CMP: case IrOp::CMN: case IrOp::TST: case IrOp::TEQ:
        case IrOp::B: case IrOp::BX: case IrOp::MSR:
            return false;
        case IrOp::STM:
            return ins.block.writeback && ins.block.rn == reg;
        case IrOp::LDM:
            return (ins.block.reg_list & (uint16_t{1} << reg)) != 0 ||
                   (ins.block.writeback && ins.block.rn == reg);
        case IrOp::STR: case IrOp::STRB: case IrOp::STRH:
            return ins.mem.writeback && ins.mem.rn == reg;
        case IrOp::LDR: case IrOp::LDRB: case IrOp::LDRH:
        case IrOp::LDRSB: case IrOp::LDRSH:
            return ins.rd == reg || (ins.mem.writeback && ins.mem.rn == reg);
        case IrOp::UMULL: case IrOp::UMLAL: case IrOp::SMULL: case IrOp::SMLAL:
            return ins.rd == reg || ins.rn == reg;
        case IrOp::Undefined:
            return true;
        default:
            return ins.rd == reg;
    }
}

// Diagnostic toggle: GBARECOMP_NO_JT=1 disables automatic jump-table
// detection, so a run can isolate the detector's cost from the baseline
// finder. Read once.
bool jump_table_detection_enabled() {
    static const bool enabled = (std::getenv("GBARECOMP_NO_JT") == nullptr);
    return enabled;
}

// Diagnostic toggle: GBARECOMP_JT_REPORT=1 prints, to stderr, every
// confirmed jump table and the emitter's decision (emitted N / rejected
// why). Used to measure how many confirmed tables the count-terminator
// actually emits — the MC-HP-000 coverage metric. Read once.
bool jt_report_enabled() {
    static const bool enabled = (std::getenv("GBARECOMP_JT_REPORT") != nullptr);
    return enabled;
}

}  // namespace

FunctionFinder::FunctionFinder(const uint8_t* rom_bytes,
                                std::size_t rom_size,
                                uint32_t rom_base)
    : rom_(rom_bytes), rom_size_(rom_size), rom_base_(rom_base) {}

void FunctionFinder::add_seed(const FunctionSeed& seed) {
    if (seed.alias_candidate) {
        uint64_t key = (static_cast<uint64_t>(seed.addr) << 1) |
                       (seed.mode == CpuMode::Thumb ? 1u : 0u);
        alias_candidate_keys_.insert(key);
    }
    seeds_.push_back(seed);
}

void FunctionFinder::add_thumb_relative_table(uint32_t addr, uint32_t stride,
                                             uint32_t count,
                                             uint32_t target_base) {
    thumb_relative_tables_.push_back({addr, stride, count, target_base});
    add_data_range(addr, addr + stride * count, "Thumb relative jump_table");
}

void FunctionFinder::add_data_range(uint32_t start, uint32_t end,
                                     const std::string& note) {
    data_ranges_.push_back(DataRange{start, end, note});
}

void FunctionFinder::add_code_copy(uint32_t runtime_start,
                                    uint32_t source_start,
                                    uint32_t size,
                                    const std::string& note) {
    code_copies_.push_back(CodeCopyRange{
        runtime_start, source_start, size, note});
}

void FunctionFinder::add_exclude(uint32_t addr,
                                  const std::string& reason) {
    excluded_.insert(addr);
    exclude_reasons_[addr] = reason;
}

bool FunctionFinder::map_addr_to_source(uint32_t addr,
                                         uint32_t* source) const {
    // Explicit fixed mappings take precedence even when their runtime address
    // lies in the nominal ROM window. Matrix Memory movie carts power on with
    // physical 0x00000200..0x000011FF paged over virtual
    // 0x08001000..0x08001FFF, so the bytes stored at the runtime file offset
    // are not the instructions the CPU sees. Ordinary RAM code copies are
    // unchanged by this ordering.
    for (const auto& cc : code_copies_) {
        if (addr >= cc.runtime_start &&
            addr - cc.runtime_start < cc.size) {
            uint32_t mapped = cc.source_start + (addr - cc.runtime_start);
            if (!addr_in_rom(mapped)) return false;
            *source = mapped;
            return true;
        }
    }
    if (addr_in_rom(addr)) {
        *source = addr;
        return true;
    }
    return false;
}

bool FunctionFinder::can_read_at(uint32_t addr, uint32_t len) const {
    uint32_t source = 0;
    if (!map_addr_to_source(addr, &source)) return false;
    return (source - rom_base_ + len) <= rom_size_;
}

bool FunctionFinder::addr_in_data_range(uint32_t addr) const {
    for (const auto& dr : data_ranges_) {
        if (addr >= dr.start && addr < dr.end) return true;
    }
    return false;
}

const DataRange* FunctionFinder::find_data_range(uint32_t addr) const {
    for (const auto& dr : data_ranges_) {
        if (addr >= dr.start && addr < dr.end) return &dr;
    }
    return nullptr;
}

void FunctionFinder::record_collision(uint32_t target,
                                       uint32_t origin_addr,
                                       const std::string& origin_name,
                                       const std::string& origin_kind) {
    // Speculative (literal-pool) flow into a data_range is NOT a hard
    // error — it just means the harvested literal wasn't really a code
    // pointer. Swallow it and mark the function for drop; discover_one
    // erases it so a genuine path can rediscover the address later.
    // Authoritative walks (entry/symbols/jump_table) still hard-error.
    if (current_walk_speculative_) {
        current_walk_dropped_ = true;
        return;
    }
    DataRangeCollision col{};
    col.flow_target_addr  = target;
    col.flow_origin_addr  = origin_addr;
    col.flow_origin_name  = origin_name;
    col.flow_origin_kind  = origin_kind;
    if (const DataRange* dr = find_data_range(target)) {
        col.range_start = dr->start;
        col.range_end   = dr->end;
        col.range_note  = dr->note;
    }
    collisions_.push_back(std::move(col));
}

uint32_t FunctionFinder::read_u32(uint32_t addr) const {
    uint32_t source = addr;
    map_addr_to_source(addr, &source);
    std::size_t off = source - rom_base_;
    return  static_cast<uint32_t>(rom_[off])
         | (static_cast<uint32_t>(rom_[off + 1]) << 8)
         | (static_cast<uint32_t>(rom_[off + 2]) << 16)
         | (static_cast<uint32_t>(rom_[off + 3]) << 24);
}

uint32_t FunctionFinder::read_u32_public(uint32_t addr) const {
    if (!can_read_at(addr, 4)) return 0;
    return read_u32(addr);
}

uint16_t FunctionFinder::read_u16(uint32_t addr) const {
    uint32_t source = addr;
    map_addr_to_source(addr, &source);
    std::size_t off = source - rom_base_;
    return  static_cast<uint16_t>(rom_[off])
         | (static_cast<uint16_t>(rom_[off + 1]) << 8);
}

bool FunctionFinder::looks_like_code(uint32_t addr, CpuMode mode,
                                     bool strict) const {
    if (mode == CpuMode::Thumb) {
        if (addr & 1u) return false;
    } else if (addr & 3u) {
        return false;
    }
    // Runtime code-copy addresses are valid code locations too; can_read_at()
    // resolves them back to their ROM backing.
    if (!can_read_at(addr, mode == CpuMode::Thumb ? 2u : 4u) ||
        addr_in_data_range(addr)) {
        return false;
    }

    const uint32_t count = strict ? 6u : 1u;
    uint32_t pc = addr;
    for (uint32_t i = 0; i < count; ++i) {
        if (mode == CpuMode::Thumb) {
            if (!can_read_at(pc, 2)) return i > 0;
            if (armv4t::ThumbDecoder::decode(read_u16(pc), pc).is_undefined)
                return false;
            pc += 2;
        } else {
            if (!can_read_at(pc, 4)) return i > 0;
            if (armv4t::ArmDecoder::decode(read_u32(pc), pc).is_undefined)
                return false;
            pc += 4;
        }
    }
    return true;
}

bool FunctionFinder::is_arm_func_prologue(uint32_t addr) const {
    uint32_t pc = addr;
    for (int i = 0; i < 2; ++i) {
        if (!can_read_at(pc, 4)) return false;
        armv4t::Instr ins = armv4t::ArmDecoder::decode(read_u32(pc), pc);
        if (ins.op == armv4t::IrOp::STM && !ins.block.load &&
            ins.block.writeback && ins.block.rn == 13 &&
            ins.block.pre_indexed && !ins.block.add &&
            (ins.block.reg_list & (1u << 14)) != 0) {
            return true;
        }
        pc += 4;
    }
    return false;
}

bool FunctionFinder::is_thumb_func_prologue(uint32_t addr) const {
    if (!can_read_at(addr, 2)) return false;
    // PUSH {...,lr}. GCC/agbcc-generated non-leaf THUMB functions use this
    // highly distinctive entry shape; leaf functions are recovered from
    // pointer tables rooted by these strong candidates.
    return (read_u16(addr) & 0xFF00u) == 0xB500u;
}

void FunctionFinder::harvest_aot_reference(uint32_t raw) {
    // Reachable literals/ADR can pass a callback or a copy source to a helper.
    // Copy sources need not carry the Thumb tag. Qualify their PUSH-LR entry
    // and instruction bytes even when weak speculative literals are disabled.
    // Discover the source CFG too: copied dispatchers can select absolute ROM
    // case labels, which must remain available outside the guarded RAM catalog.
    const bool bounded = aot_scan_start_ < aot_scan_end_;
    const uint32_t target = raw & ~1u;
    if ((!bounded || (target >= aot_scan_start_ &&
                      uint64_t(target) + 12u <= aot_scan_end_)) &&
        is_thumb_func_prologue(target) &&
        looks_like_code(target, CpuMode::Thumb, true) &&
        aot_seen_seed_keys_.insert(visit_key(target, CpuMode::Thumb)).second) {
        aot_pointer_seeds_.push_back({target, CpuMode::Thumb, "", 0, true});
        ++stats_.aot_callback_seeds;
    }
    // Table/descriptor searches still require an explicitly executable range.
    if (!bounded || (raw & 1u)) return;
    const uint32_t table_base = raw;
    if ((table_base & 3u) == 0u && addr_in_rom(table_base) &&
        relative_callback_bases_.insert(table_base).second)
        relative_callbacks_dirty_ = true;
    if ((table_base & 3u) != 0u ||
        !addr_in_rom(table_base) ||
        !aot_seen_pointer_tables_.insert(table_base).second) {
        return;
    }

    struct PointerWord {
        uint32_t raw;
        uint32_t target;
    };
    std::vector<PointerWord> entries;
    entries.reserve(32);
    for (uint32_t i = 0; i < 1024u; ++i) {
        const uint32_t pos = table_base + i * 4u;
        if (!can_read_at(pos, 4)) break;
        const uint32_t raw = read_u32(pos);
        const uint32_t target = raw & ~uint32_t{1};
        const bool aligned =
            (raw & 1u) != 0u || (raw & 3u) == 0u;
        if (!aligned || target < aot_scan_start_ ||
            target >= aot_scan_end_) {
            break;
        }
        entries.push_back(PointerWord{raw, target});
    }
    if (entries.size() < 2) return;

    std::size_t odd_thumb_evidence = 0;
    std::size_t thumb_prologues = 0;
    std::size_t arm_prologues = 0;
    std::unordered_set<uint32_t> unique_targets;
    std::unordered_set<uint32_t> odd_thumb_targets;
    std::unordered_set<uint32_t> thumb_prologue_targets;
    std::unordered_set<uint32_t> arm_prologue_targets;
    for (const auto& p : entries) {
        unique_targets.insert(p.target);
        if ((p.raw & 1u) &&
            looks_like_code(p.target, CpuMode::Thumb, true)) {
            odd_thumb_targets.insert(p.target);
        }
        if (is_thumb_func_prologue(p.target) &&
            looks_like_code(p.target, CpuMode::Thumb, true)) {
            thumb_prologue_targets.insert(p.target);
        } else if (is_arm_func_prologue(p.target) &&
                   looks_like_code(p.target, CpuMode::Arm, true)) {
            arm_prologue_targets.insert(p.target);
        }
    }
    odd_thumb_evidence = odd_thumb_targets.size();
    thumb_prologues = thumb_prologue_targets.size();
    arm_prologues = arm_prologue_targets.size();

    const bool interworking =
        odd_thumb_evidence >= 2 &&
        odd_thumb_evidence * 2 >= unique_targets.size();
    CpuMode uniform_mode = CpuMode::Thumb;
    bool uniform = false;
    if (!interworking && thumb_prologues >= 2 &&
        thumb_prologues * 2 >= unique_targets.size() &&
        arm_prologues == 0) {
        uniform = true;
        uniform_mode = CpuMode::Thumb;
    } else if (!interworking && arm_prologues >= 2 &&
               arm_prologues * 2 >= unique_targets.size() &&
               thumb_prologues == 0) {
        uniform = true;
        uniform_mode = CpuMode::Arm;
    }
    if (!interworking && !uniform) return;

    std::size_t emitted = 0;
    for (const auto& p : entries) {
        CpuMode mode = uniform_mode;
        if (interworking)
            mode = (p.raw & 1u) ? CpuMode::Thumb : CpuMode::Arm;
        if (!looks_like_code(p.target, mode, true)) continue;
        if (mode == CpuMode::Arm && !is_arm_func_prologue(p.target))
            continue;

        const uint64_t key = visit_key(p.target, mode);
        if (!aot_seen_seed_keys_.insert(key).second) continue;
        aot_pointer_seeds_.push_back(FunctionSeed{
            p.target, mode, "", 0, /*speculative=*/true});
        if ((mode == CpuMode::Thumb && is_thumb_func_prologue(p.target)) ||
            (mode == CpuMode::Arm && is_arm_func_prologue(p.target))) {
            ++stats_.aot_prologue_seeds;
        } else {
            ++stats_.aot_pointer_seeds;
        }
        ++emitted;
    }
    if (emitted != 0) ++stats_.aot_pointer_tables;
}

void FunctionFinder::harvest_relative_callbacks() {
    if (!relative_callbacks_dirty_) return;
    relative_callbacks_dirty_ = false;
    if (relative_callback_slots_.count(0) == 0 || relative_callback_slots_.size() < 2)
        return;
    std::vector<uint32_t> bases(relative_callback_bases_.begin(), relative_callback_bases_.end());
    std::vector<uint32_t> slots(relative_callback_slots_.begin(), relative_callback_slots_.end());
    std::sort(bases.begin(),bases.end());
    std::sort(slots.begin(),slots.end());
    for (uint32_t base : bases) {
        if (!can_read_at(base,4)) continue;
        const uint32_t first = base + read_u32(base);
        const auto strong = [&](uint32_t raw) {
            const auto target=raw & ~1u;
            return (raw & 1u) && target >= aot_scan_start_ &&
                uint64_t(target)+12u <= aot_scan_end_ &&
                is_thumb_func_prologue(target) &&
                looks_like_code(target,CpuMode::Thumb,true);
        };
        if (!strong(first)) continue;
        std::vector<uint32_t> targets;
        std::unordered_set<uint32_t> prologues;
        for (uint32_t slot : slots) {
            if (!can_read_at(base+slot,4)) continue;
            const uint32_t raw=base+read_u32(base+slot);
            const uint32_t target=raw & ~1u;
            if (!(raw & 1u) || target < aot_scan_start_ || target >= aot_scan_end_)
                continue;
            if (strong(raw)) {
                prologues.insert(target);
                targets.push_back(target);
            } else if (can_read_at(target,2) && read_u16(target)==0x4770u) {
                targets.push_back(target); // exact BX LR leaf
            }
        }
        // Only offsets actually consumed by a relative callback dispatcher are
        // read. Two independent PUSH-LR entries corroborate the referenced base;
        // there is no scan for a guessed table end or adjacent-table contents.
        if (prologues.size()<2) continue;
        for (uint32_t target : targets) {
            if (!aot_seen_seed_keys_.insert(visit_key(target,CpuMode::Thumb)).second)
                continue;
            aot_pointer_seeds_.push_back({target,CpuMode::Thumb,"",0,true});
            ++stats_.aot_callback_seeds;
        }
    }
}

bool FunctionFinder::is_thumb_ram_leaf(uint32_t addr, uint32_t* ram_global) const {
    if (!looks_like_code(addr, CpuMode::Thumb, false)) return false;
    const auto first = armv4t::ThumbDecoder::decode(read_u16(addr), addr);
    if (first.op != armv4t::IrOp::LDR || first.rd >= 4 ||
        first.mem.rn != 15 || first.mem.by_register || first.mem.writeback)
        return false;
    const uint32_t literal = ((addr + 4u) & ~3u) + first.mem.imm_offset;
    if (!can_read_at(literal, 4)) return false;
    const uint32_t ram = read_u32(literal);
    if (ram < 0x02000000u || ram >= 0x04000000u) return false;

    // A small, stackless leaf anchored by a RAM-global literal is a stronger
    // entry shape than merely decoding arbitrary Thumb bytes. Require a store
    // and BX LR, with no calls, other branches, SP, or callee-saved registers.
    bool stored = false;
    for (uint32_t pc = addr + 2u; uint64_t(pc) < uint64_t(addr) + 64u; pc += 2u) {
        if (pc >= aot_scan_end_ || !can_read_at(pc, 2) || addr_in_data_range(pc))
            return false;
        const auto ins = armv4t::ThumbDecoder::decode(read_u16(pc), pc);
        if (ins.is_undefined) return false;
        if (ins.op == armv4t::IrOp::BX && ins.rm == 14) {
            if (stored && ram_global) *ram_global = ram;
            return stored;
        }
        if (ins.is_branch || ins.is_call || ins.rd >= 4) return false;
        switch (ins.op) {
            case armv4t::IrOp::LDR: case armv4t::IrOp::LDRB:
            case armv4t::IrOp::LDRH: case armv4t::IrOp::LDRSB:
            case armv4t::IrOp::LDRSH:
            case armv4t::IrOp::STR: case armv4t::IrOp::STRB:
            case armv4t::IrOp::STRH:
                if ((ins.mem.rn >= 4 && ins.mem.rn != 15) ||
                    (ins.mem.by_register && ins.mem.reg_offset.rm >= 4) ||
                    ins.mem.writeback) return false;
                if (ins.op == armv4t::IrOp::STR ||
                    ins.op == armv4t::IrOp::STRB || ins.op == armv4t::IrOp::STRH)
                    stored = true;
                break;
            case armv4t::IrOp::ADD: case armv4t::IrOp::SUB:
            case armv4t::IrOp::RSB: case armv4t::IrOp::ADC:
            case armv4t::IrOp::SBC: case armv4t::IrOp::AND:
            case armv4t::IrOp::EOR: case armv4t::IrOp::ORR:
            case armv4t::IrOp::BIC: case armv4t::IrOp::MOV:
            case armv4t::IrOp::MVN:
                if (ins.rn >= 4 ||
                    (ins.op2.kind == armv4t::Op2::Kind::Shifted &&
                     (ins.op2.shifted.rm >= 4 ||
                      (ins.op2.shifted.by_register && ins.op2.shifted.imm_or_rs >= 4))))
                    return false;
                break;
            default:
                return false;
        }
    }
    return false;
}

std::vector<FunctionSeed> FunctionFinder::collect_referenced_callback_seeds() {
    std::vector<FunctionSeed> out;
    if (aot_scan_start_ >= aot_scan_end_) return out;
    std::unordered_set<uint32_t> objects;
    std::unordered_set<uint32_t> referenced_code;
    std::unordered_set<uint32_t> callback_globals;
    std::unordered_set<uint32_t> store_globals;
    std::unordered_set<uint64_t> emitted;
    for (const auto& seed : seeds_) emitted.insert(visit_key(seed.addr, seed.mode));
    std::unordered_map<uint32_t, uint32_t> leaf_globals;
    auto leaf_global = [&](uint32_t pc) {
        const auto found = leaf_globals.find(pc);
        if (found != leaf_globals.end()) return found->second;
        uint32_t ram = 0;
        is_thumb_ram_leaf(pc, &ram);
        leaf_globals.emplace(pc, ram);
        return ram;
    };
    auto emit_callback = [&](uint32_t pc) {
        if (!emitted.insert(visit_key(pc, CpuMode::Thumb)).second) return;
        out.push_back({pc, CpuMode::Thumb, "", 0, /*speculative=*/true});
        aot_seen_seed_keys_.insert(visit_key(pc, CpuMode::Thumb));
        ++stats_.aot_callback_seeds;
    };
    auto anchor_callback = [&](uint32_t pc) {
        const auto roots=thumb_ram_store_roots(pc);
        store_globals.insert(roots.begin(),roots.end());
        emit_callback(pc);
    };

    // Referenced data objects need two tagged Thumb entries, including a
    // PUSH-LR entry corroborating another prologue or a RAM-global leaf.
    // Executable scan spans can also contain callback records (including a
    // conservative whole-ROM span). Without an authoritative data range,
    // require four distinct PUSH-LR entries there instead of treating an
    // arbitrary adjacent pair of instruction words as a descriptor.
    // No table-end guess or unrestricted harvesting of pointer-shaped words.
    for (std::size_t off = 0; off + 4 <= rom_size_; off += 4) {
        const uint32_t object = read_u32(rom_base_ + static_cast<uint32_t>(off));
        const uint32_t code = object & ~1u;
        if ((object & 1u) && code >= aot_scan_start_ && code < aot_scan_end_)
            referenced_code.insert(code);
        if ((object & 3u) || !addr_in_rom(object) ||
            !objects.insert(object).second) continue;
        const auto* data = find_data_range(object);
        const bool declared_data = data && uint64_t(object) + 8u <= data->end;
        const bool executable = object >= aot_scan_start_ && object < aot_scan_end_;
        // Some referenced descriptor arrays begin with an optional NULL pair
        // and use long frameless setters. A fixed four-record witness can
        // corroborate these without inferring an array's extent: every record
        // must be NULL/NULL or a return-proved RAM setter plus a frame or RAM
        // leaf callback,
        // with two distinct setters and two distinct frame entries overall.
        // An arbitrary executable-span pair still cannot qualify on its own.
        if (executable && !declared_data && can_read_at(object, 32u)) {
            std::set<uint32_t> leaves, frames, leaf_updates, globals;
            bool valid = true;
            for (unsigned record = 0; record < 4u; ++record) {
                const uint32_t raw_leaf = read_u32(object + record*8u);
                const uint32_t raw_frame = read_u32(object + record*8u + 4u);
                if (!raw_leaf && !raw_frame) continue;
                const uint32_t leaf = raw_leaf & ~1u, frame = raw_frame & ~1u;
                if (!(raw_leaf & 1u) || !(raw_frame & 1u) ||
                    leaf < aot_scan_start_ || leaf >= aot_scan_end_ ||
                    frame < aot_scan_start_ || uint64_t(frame)+12u > aot_scan_end_) {
                    valid = false; break;
                }
                const uint32_t ram = leaf_global(leaf);
                if (!ram) { valid = false; break; }
                leaves.insert(leaf); globals.insert(ram);
                if (is_thumb_func_prologue(frame) && looks_like_code(frame, CpuMode::Thumb, true))
                    frames.insert(frame);
                else if (const uint32_t update_ram = leaf_global(frame)) {
                    leaf_updates.insert(frame); globals.insert(update_ram);
                } else { valid = false; break; }
            }
            if (valid && leaves.size() >= 2u && frames.size() >= 2u) {
                ++stats_.aot_callback_objects;
                callback_globals.insert(globals.begin(), globals.end());
                for (uint32_t pc : leaves) anchor_callback(pc);
                for (uint32_t pc : frames) anchor_callback(pc);
                for (uint32_t pc : leaf_updates) anchor_callback(pc);
            }
        }
        const unsigned fields = executable && !declared_data ? 4u : 2u;
        if (!can_read_at(object, fields * 4u)) continue;
        uint32_t target[4] = {};
        uint32_t ram_global[4] = {};
        bool push[4] = {}, leaf[4] = {}, all_push = true;
        bool valid = true;
        for (unsigned i = 0; i < fields; ++i) {
            const uint32_t raw = read_u32(object + 4u * i);
            target[i] = raw & ~1u;
            if (!(raw & 1u) || target[i] < aot_scan_start_ ||
                target[i] >= aot_scan_end_ ||
                static_cast<uint64_t>(target[i]) + 12u > aot_scan_end_ ||
                !looks_like_code(target[i], CpuMode::Thumb, true)) {
                valid = false;
                break;
            }
            push[i] = is_thumb_func_prologue(target[i]);
            leaf[i] = !push[i] && is_thumb_ram_leaf(target[i], &ram_global[i]);
            all_push = all_push && push[i];
        }
        if (!valid || target[0] == target[1] || !(push[0] || push[1]) ||
            !(push[0] || leaf[0]) || !(push[1] || leaf[1])) continue;
        if (fields == 4 && (!all_push ||
            std::unordered_set<uint32_t>(target, target + fields).size() != fields))
            continue;
        ++stats_.aot_callback_objects;
        for (unsigned i = 0; i < fields; ++i) {
            const uint32_t ram = ram_global[i];
            if (ram != 0) callback_globals.insert(ram);
        }
        for (unsigned i = 0; i < fields; ++i) {
            const uint32_t pc = target[i];
            anchor_callback(pc);
        }
    }
    // Independent records may share the descriptor's already corroborated
    // RAM-global setter contract. Validate each exact pair separately; do not
    // scan from a base to a guessed terminator or classify intervening data.
    if (!callback_globals.empty()) for (std::size_t off=0;off+8u<=rom_size_;off+=4u) {
        const uint32_t slot=rom_base_+static_cast<uint32_t>(off);
        const uint32_t raw_leaf=read_u32(slot), raw_frame=read_u32(slot+4u);
        const uint32_t leaf=raw_leaf & ~1u, frame=raw_frame & ~1u;
        if (!(raw_leaf & 1u) || !(raw_frame & 1u) || leaf==frame ||
            leaf<aot_scan_start_ || leaf>=aot_scan_end_ ||
            frame<aot_scan_start_ || uint64_t(frame)+12u>aot_scan_end_ ||
            !is_thumb_func_prologue(frame) ||
            !looks_like_code(frame,CpuMode::Thumb,true)) continue;
        const uint32_t ram=leaf_global(leaf);
        if (!ram || !callback_globals.count(ram)) continue;
        emit_callback(leaf); emit_callback(frame);
    }
    // A script may also take setters individually rather than through the
    // descriptor. Require the same bounded leaf shape and the exact RAM-global
    // literal already corroborated by a descriptor, plus an odd ROM reference.
    std::vector<uint32_t> ordered_code(referenced_code.begin(), referenced_code.end());
    std::sort(ordered_code.begin(), ordered_code.end());
    std::unordered_set<uint32_t> store_callbacks;
    auto return_proved = [&](uint32_t pc) {
        if(is_thumb_callback_leaf(pc)) return true;
        if(!is_thumb_func_prologue(pc) || !looks_like_code(pc,CpuMode::Thumb,true)) return false;
        const auto effects=thumb_call_effects(pc);
        return effects.valid && !effects.writes_caller_stack && (effects.preserved & 0x2FF0u)==0x2FF0u;
    };
    std::unordered_set<uint32_t> deferred;
    auto defer_callback = [&](uint32_t pc) {
        if(emitted.count(visit_key(pc,CpuMode::Thumb)) || !deferred.insert(pc).second) return;
        deferred_store_callbacks_.push_back({pc,CpuMode::Thumb,"",0,true});
    };
    for (uint32_t pc : ordered_code) {
        const uint32_t ram=leaf_global(pc);
        if(ram && callback_globals.count(ram)) emit_callback(pc); // existing contract
        const auto roots=thumb_ram_store_roots(pc);
        const bool shared=std::any_of(roots.begin(),roots.end(),[&](uint32_t root) {
            return store_globals.count(root)!=0;
        });
        if(shared && return_proved(pc)) {
            store_callbacks.insert(pc);
            defer_callback(pc);
        }
    }
    // An exact tagged record may pair a proved shared-global callback with a
    // return-proved update, including BX LR. This does not propagate globals
    // from new callbacks or recursively qualify adjacent pointer-shaped words.
    if(!store_callbacks.empty()) for(std::size_t off=0;off+8u<=rom_size_;off+=4u) {
        const uint32_t slot=rom_base_+static_cast<uint32_t>(off);
        const uint32_t first=read_u32(slot),second=read_u32(slot+4u),target=second & ~1u;
        if(!(first & 1u) || !(second & 1u) || first==second ||
           !store_callbacks.count(first & ~1u) || target<aot_scan_start_ ||
           uint64_t(target)+2u>aot_scan_end_ || addr_in_data_range(target)) continue;
        if(return_proved(target)) defer_callback(target);
    }
    return out;
}

std::vector<FunctionSeed> FunctionFinder::collect_code_copy_seeds() const {
    std::vector<FunctionSeed> out;
    std::unordered_set<uint64_t> emitted;
    for (const auto& seed : seeds_)
        emitted.insert(visit_key(seed.addr, seed.mode));
    for (const auto& cc : code_copies_) {
        bool scan_arm = false;
        bool scan_thumb = false;
        for (const auto& seed : seeds_) {
            if (seed.addr < cc.runtime_start ||
                seed.addr - cc.runtime_start >= cc.size) {
                continue;
            }
            if (seed.mode == CpuMode::Arm) scan_arm = true;
            else                           scan_thumb = true;
        }
        if (scan_arm) {
            uint32_t pc = (cc.runtime_start + 3u) & ~uint32_t{3};
            const uint32_t end = cc.runtime_start + cc.size;
            for (; pc + 4u <= end; pc += 4u) {
                if (!can_read_at(pc, 4)) break;
                armv4t::Instr ins =
                    armv4t::ArmDecoder::decode(read_u32(pc), pc);
                const bool stack_push =
                    ins.op == armv4t::IrOp::STM && !ins.block.load &&
                    ins.block.writeback && ins.block.rn == 13 &&
                    ins.block.pre_indexed && !ins.block.add &&
                    ins.block.reg_list != 0;
                if (!stack_push) {
                    continue;
                }

                // Old APCS ARM functions commonly begin with `mov ip,sp`
                // immediately before their stack push. The callable entry is
                // the MOV, not the push itself; rooting the latter misses
                // address-taken helpers such as SMA2's copied sound mixer.
                uint32_t entry = pc;
                if (pc >= cc.runtime_start + 4u &&
                    read_u32(pc - 4u) == 0xE1A0C00Du) {
                    entry = pc - 4u;
                }
                if (!looks_like_code(entry, CpuMode::Arm, true)) continue;

                const uint64_t key = visit_key(entry, CpuMode::Arm);
                if (!emitted.insert(key).second) continue;
                FunctionSeed seed{
                    entry, CpuMode::Arm, "", 0, /*speculative=*/true};
                seed.source_addr =
                    cc.source_start + (entry - cc.runtime_start);
                out.push_back(std::move(seed));
            }
        }

        if (scan_thumb) {
            uint32_t pc = (cc.runtime_start + 1u) & ~uint32_t{1};
            const uint32_t end = cc.runtime_start + cc.size;
            for (; pc + 2u <= end; pc += 2u) {
                if (!can_read_at(pc, 2)) break;
                const uint16_t op = read_u16(pc);
                const bool stack_push =
                    (op & 0xFE00u) == 0xB400u &&
                    (op & 0x01FFu) != 0;
                if (!stack_push ||
                    !looks_like_code(pc, CpuMode::Thumb, true)) {
                    continue;
                }
                const uint64_t key = visit_key(pc, CpuMode::Thumb);
                if (!emitted.insert(key).second) continue;
                FunctionSeed seed{
                    pc, CpuMode::Thumb, "", 0, /*speculative=*/true};
                seed.source_addr =
                    cc.source_start + (pc - cc.runtime_start);
                out.push_back(std::move(seed));
            }
        }
    }
    return out;
}

void FunctionFinder::discover_one(uint32_t entry_addr, CpuMode entry_mode,
                                   const std::string& seed_name,
                                   uint32_t seed_source_addr,
                                   bool speculative) {
    uint32_t entry_source = 0;
    if (seed_source_addr != 0) {
        entry_source = seed_source_addr;
        if (!addr_in_rom(entry_source)) return;
    } else if (!map_addr_to_source(entry_addr, &entry_source)) {
        return;
    }

    // An explicit source address describes one observed placement of a
    // position-independent ROM routine in RAM. Fixed code_copy ranges may
    // overlap that placement (stack copies naturally move as call depth
    // changes), so instruction fetches for this walk must retain the seed's
    // source/runtime bias instead of consulting the first global range again.
    auto walk_source_for = [&](uint32_t addr, uint32_t* source) -> bool {
        const int64_t delta = static_cast<int64_t>(addr) -
            static_cast<int64_t>(entry_addr);
        const int64_t mapped = static_cast<int64_t>(entry_source) + delta;
        if (mapped < 0 || mapped > 0xFFFFFFFFll) return false;
        *source = static_cast<uint32_t>(mapped);
        return addr_in_rom(*source);
    };
    auto can_read_walk_at = [&](uint32_t addr, uint32_t len) -> bool {
        uint32_t source = 0;
        if (!walk_source_for(addr, &source)) return false;
        return static_cast<uint64_t>(source - rom_base_) + len <= rom_size_;
    };
    auto read_walk_u16 = [&](uint32_t addr) -> uint16_t {
        uint32_t source = 0;
        walk_source_for(addr, &source);
        const std::size_t off = source - rom_base_;
        return static_cast<uint16_t>(rom_[off]) |
            (static_cast<uint16_t>(rom_[off + 1]) << 8);
    };
    auto read_walk_u32 = [&](uint32_t addr) -> uint32_t {
        uint32_t source = 0;
        walk_source_for(addr, &source);
        const std::size_t off = source - rom_base_;
        return static_cast<uint32_t>(rom_[off]) |
            (static_cast<uint32_t>(rom_[off + 1]) << 8) |
            (static_cast<uint32_t>(rom_[off + 2]) << 16) |
            (static_cast<uint32_t>(rom_[off + 3]) << 24);
    };
    uint64_t key = visit_key(entry_addr, entry_mode);
    if (visited_.find(key) != visited_.end()) return;

    // Speculative (literal-pool) walks downgrade data_range collisions
    // from a hard error to a silent drop (see record_collision).
    current_walk_speculative_ = speculative;
    current_walk_dropped_ = false;

    Function fn{};
    fn.addr = entry_addr;
    fn.source_addr = entry_source;
    fn.mode = entry_mode;
    fn.name = seed_name.empty() ? default_name(entry_addr, entry_mode)
                                : seed_name;
    fn.end_addr = entry_addr;  // updated below

    visited_[key] = functions_.size();  // index will be re-sorted at end

    uint32_t pc = entry_addr;
    const uint32_t step = (entry_mode == CpuMode::Thumb) ? 2u : 4u;
    constexpr std::size_t kMaxInstrs = 4096;
    std::size_t count = 0;

    // Narrow intra-function constant tracker. Lets the finder
    // follow the classic ARM↔THUMB interworking pattern:
    //   add R0, PC, #1
    //   bx  R0
    // (and the THUMB equivalent: `mov Rn, pc; bx Rn`). When we see
    // BX with rm != 14 (non-return), we look up Rm in this table.
    // If it has a tracked value, we add it as a branch target
    // (with mode inferred from the low bit, like the SWI table's
    // entries_mode="auto").
    //
    // Coverage is deliberately narrow:
    //   - tracks `MOV Rn, #imm`  (clears the rest)
    //   - tracks `ADD Rn, PC, #imm`  (computes PC+8+imm for ARM,
    //     PC+4+imm for THUMB)
    //   - any other write to Rn invalidates the entry
    //   - any control-flow break (branch, call) wipes the table
    //
    // False positives are avoided by being conservative: we only
    // resolve a BX target when we're certain we saw the constant
    // load IN THIS BASIC BLOCK with no intervening write to Rm.
    struct RegConst {
        bool     known = false;
        uint32_t value = 0;
    };
    RegConst reg_const[16];

    struct CopyLoopEntry {
        uint32_t pc;
        std::array<RegConst, 16> regs;
    };
    std::deque<CopyLoopEntry> copy_loop_entries;
    auto harvest_callback_copy = [&](uint32_t start, uint32_t backedge,
                                     const std::array<RegConst, 16>& initial) {
        // Simulate only a short, straight-line ROM-to-RAM word-copy loop.
        // Every operand must be known, and the actual CMP/backedge must
        // terminate within the analysis budget. The budget is a rejection
        // limit, never a guessed table length. No RAM is read or written.
        if (aot_scan_start_ >= aot_scan_end_ || start >= backedge ||
            backedge - start > 64u) return;
        std::vector<armv4t::Instr> body;
        for (uint32_t pos = start; pos <= backedge; pos += 2u) {
            if (!can_read_walk_at(pos, 2) || addr_in_data_range(pos)) return;
            auto decoded = armv4t::ThumbDecoder::decode(read_walk_u16(pos), pos);
            if (decoded.is_undefined) return;
            if (pos != backedge && (decoded.is_branch || decoded.is_call ||
                                    decoded.cond != armv4t::Cond::AL)) return;
            body.push_back(decoded);
        }
        const auto& branch = body.back();
        if (branch.op != armv4t::IrOp::B || branch.branch_target != start ||
            body.size() < 3 || body[body.size()-2].op != armv4t::IrOp::CMP)
            return;
        switch (branch.cond) {
            case armv4t::Cond::CC: case armv4t::Cond::LS:
            case armv4t::Cond::LT: case armv4t::Cond::LE:
            case armv4t::Cond::NE: break;
            default: return;
        }
        auto regs = initial;
        std::array<uint32_t, 16> origin{};
        struct CopyWord { uint32_t source, destination, value; };
        std::vector<CopyWord> words;
        bool terminated = false;
        for (unsigned iteration = 0; iteration < 256; ++iteration) {
            unsigned loads = 0, stores = 0;
            uint32_t lhs = 0, rhs = 0;
            auto get_reg = [&](uint8_t reg, uint32_t pc_value, uint32_t& value) {
                if (reg == 15) { value = (pc_value + 4u) & ~3u; return true; }
                if (reg >= 16 || !regs[reg].known) return false;
                value = regs[reg].value;
                return true;
            };
            auto operand = [&](const armv4t::Instr& op, uint32_t& value) {
                if (op.op2.kind == armv4t::Op2::Kind::Imm) {
                    value = op.op2.imm_value;
                    return true;
                }
                if (op.op2.kind != armv4t::Op2::Kind::Shifted ||
                    op.op2.shifted.by_register ||
                    !get_reg(op.op2.shifted.rm, op.pc, value)) return false;
                const unsigned shift = op.op2.shifted.imm_or_rs;
                if (shift >= 32u) return false;
                switch (op.op2.shifted.type) {
                    case armv4t::ShiftType::LSL: value <<= shift; return true;
                    case armv4t::ShiftType::LSR:
                        if (!shift) return false;
                        value >>= shift; return true;
                    case armv4t::ShiftType::ASR:
                        if (!shift) return false;
                        value = (value >> shift) | ((value & 0x80000000u)
                            ? (~uint32_t{0} << (32u-shift)) : 0u);
                        return true;
                    default: return false;
                }
            };
            auto address = [&](const armv4t::Instr& op, uint32_t& value) {
                if (!op.mem.pre_indexed || op.mem.writeback ||
                    !get_reg(op.mem.rn, op.pc, value))
                    return false;
                uint32_t offset = op.mem.imm_offset;
                if (op.mem.by_register) {
                    const auto& shift = op.mem.reg_offset;
                    if (shift.by_register || shift.type != armv4t::ShiftType::LSL ||
                        shift.imm_or_rs >= 32u || !get_reg(shift.rm, op.pc, offset))
                        return false;
                    offset <<= shift.imm_or_rs;
                }
                value = op.mem.add ? value + offset : value - offset;
                return (value & 3u) == 0;
            };
            for (std::size_t index = 0; index + 1 < body.size(); ++index) {
                const auto& op = body[index];
                uint32_t value = 0, left = 0;
                if (op.op == armv4t::IrOp::CMP) {
                    if (index + 2 != body.size() ||
                        !get_reg(op.rn, op.pc, lhs) || !operand(op, rhs)) return;
                } else if (op.op == armv4t::IrOp::STR) {
                    uint32_t destination = 0;
                    if (op.rd >= 15 || !regs[op.rd].known || !origin[op.rd] ||
                        !address(op, destination) || ++stores != 1) return;
                    const bool ram = (destination >= 0x02000000u && destination <= 0x0203FFFCu) ||
                                     (destination >= 0x03000000u && destination <= 0x03007FF8u);
                    if (!ram) return; // IRQ-vector/MMIO writes are not array copies.
                    words.push_back({origin[op.rd], destination, regs[op.rd].value});
                } else if (op.op == armv4t::IrOp::LDR) {
                    uint32_t source = 0;
                    if (op.rd >= 13 || !address(op, source) ||
                        !addr_in_rom(source) || !can_read_at(source, 4)) return;
                    regs[op.rd] = {true, read_u32(source)};
                    origin[op.rd] = op.mem.rn == 15 ? 0u : source;
                    if (origin[op.rd] && ++loads != 1) return;
                } else if (op.op == armv4t::IrOp::MOV) {
                    if (op.rd >= 13 || !operand(op, value)) return;
                    const bool copy = op.op2.kind == armv4t::Op2::Kind::Shifted &&
                        op.op2.shifted.type == armv4t::ShiftType::LSL &&
                        op.op2.shifted.imm_or_rs == 0;
                    const uint32_t source = copy ? origin[op.op2.shifted.rm] : 0u;
                    regs[op.rd] = {true, value};
                    origin[op.rd] = source;
                } else if (op.op == armv4t::IrOp::ADD || op.op == armv4t::IrOp::SUB) {
                    if (op.rd >= 13 || !get_reg(op.rn, op.pc, left) ||
                        !operand(op, value)) return;
                    regs[op.rd] = {true, op.op == armv4t::IrOp::ADD ? left + value : left - value};
                    origin[op.rd] = 0;
                } else {
                    return;
                }
            }
            if (loads != 1 || stores != 1) return;
            if (words.size() > 1) {
                const auto& prev = words[words.size()-2];
                const auto& next = words.back();
                if (uint64_t(prev.source)+4u != next.source ||
                    uint64_t(prev.destination)+4u != next.destination) return;
            }
            const uint32_t result = lhs - rhs;
            const bool n = (result >> 31) != 0, z = result == 0, c = lhs >= rhs;
            const bool v = (((lhs ^ rhs) & (lhs ^ result)) >> 31) != 0;
            bool taken = false;
            switch (branch.cond) {
                case armv4t::Cond::CC: taken = !c; break;
                case armv4t::Cond::LS: taken = !c || z; break;
                case armv4t::Cond::LT: taken = n != v; break;
                case armv4t::Cond::LE: taken = z || n != v; break;
                case armv4t::Cond::NE: taken = !z; break;
                default: return;
            }
            if (!taken) { terminated = true; break; }
        }
        if (!terminated || words.size() < 2) return;
        // Two independent PUSH-LR entries corroborate a callback array.
        // RAM/null/payload words neither seed code nor truncate its proven
        // copy extent. Leaf Thumb callbacks retain their actual pointer tag.
        std::unordered_set<uint32_t> prologues;
        for (const auto& word : words) {
            const uint32_t target = word.value & ~1u;
            if ((word.value & 1u) && target >= aot_scan_start_ &&
                uint64_t(target)+12u <= aot_scan_end_ && addr_in_rom(target) &&
                is_thumb_func_prologue(target) && looks_like_code(target, CpuMode::Thumb, true))
                prologues.insert(target);
        }
        if (prologues.size() < 2) return;
        std::size_t emitted = 0;
        for (const auto& word : words) {
            const uint32_t target = word.value & ~1u;
            if (!(word.value & 1u) || target < aot_scan_start_ ||
                uint64_t(target)+12u > aot_scan_end_ || !addr_in_rom(target) ||
                !looks_like_code(target, CpuMode::Thumb, true) ||
                !aot_seen_seed_keys_.insert(visit_key(target, CpuMode::Thumb)).second)
                continue;
            aot_pointer_seeds_.push_back({target, CpuMode::Thumb, "", 0, true});
            ++stats_.aot_callback_seeds;
            ++emitted;
        }
        if (emitted) ++stats_.aot_callback_objects;
    };

    // Jump-table address tracking (MC-HP-000). The compiler builds a
    // dispatch like:  LDR Rb,=table ; LSL Ri,Ri,#k ; ADD Ra,Ri,Rb ;
    // LDR Rt,[Ra] ; BX Rt (or BL into the bx-rN veneer). reg_const
    // already resolves Rb from its literal load; these two track the
    // scaled index and the resulting element-address register so the
    // LDR that fetches the entry can reveal the table.
    struct RegScaled {  // Rd = (unknown index) << shift
        bool     known = false;
        uint8_t  shift = 0;
        // Exact range bound carried from a `CMP index,#N; B{hi,cs}` guard
        // (max_index is the largest in-range index). Lets the jump-table
        // emitter size the table precisely instead of guessing.
        bool     bound_known = false;
        uint32_t max_index   = 0;
        // The source index register this scale was derived from (the Rm of
        // the `LSL`). When the switch guard's `CMP index,#N` comes AFTER the
        // scale (common — the index is scaled into a scratch reg, e.g.
        // `lsl r0,r4,#2; mov ip,r0; ... cmp r4,#8`), bound_known is still
        // false at scale time; the table-former re-checks reg_bound[src_reg]
        // so the late bound still sizes the table. 0xFF = no known source.
        uint8_t  src_reg = 0xFF;
        uint64_t relative_bound_epoch = 0;
        bool relative_source_live = false;
    };
    RegScaled reg_scaled[16];
    struct RegTableAddr {  // Rd = base + (scaled index)
        bool     known  = false;
        uint32_t base   = 0;
        uint32_t stride = 0;
        bool     bound_known = false;  // carried from RegScaled
        uint32_t max_index   = 0;
    };
    RegTableAddr reg_table[16];

    // Range bound for a register established by a `CMP Ri,#N` switch
    // guard. The bound travels with the index through the scale/add chain
    // and gives the jump-table emitter an EXACT entry count — the precise
    // terminator that replaces the old "stop at first non-prologue" walk.
    // Two guard shapes feed it: the in-block `B{hi,cs}`-to-default form
    // (bound set here as we walk past it), and the inverted
    // `B{ls,cc}`-to-dispatch form, where this walk IS the dispatch target
    // and the bound was parked in branch_target_bounds_ by the predecessor.
    struct RegBound {
        bool     known     = false;
        uint32_t max_index = 0;
    };
    RegBound reg_bound[16];
    // Relative-table proofs cannot survive a call: argument registers and
    // flags may be clobbered. Track provenance separately so this narrow
    // recognizer does not change the existing abs32 detector's behavior.
    uint64_t relative_epoch = 1;
    uint64_t relative_bound_epoch[16] = {};
    uint64_t relative_base_epoch[16] = {};
    auto invalidate_relative_source = [&](uint8_t reg) {
        for (auto& scaled : reg_scaled) {
            if (scaled.src_reg == reg) scaled.relative_source_live = false;
        }
    };
    // Inverted-guard bound parked for this seed (see header). Re-seed it
    // so the dispatch's scale->add->load chain can size the table.
    {
        auto it = branch_target_bounds_.find(entry_addr);
        if (it != branch_target_bounds_.end() && it->second.reg < 16) {
            reg_bound[it->second.reg] = {true, it->second.max_index};
            relative_bound_epoch[it->second.reg] = relative_epoch;
        }
    }
    struct LastCmp {  // most recent `CMP Ri,#imm`, consumed by the next branch
        bool     known = false;
        uint8_t  reg   = 0;
        uint32_t imm   = 0;
        uint32_t pc    = 0;
    };
    LastCmp last_cmp;

    // A jump-table candidate awaiting confirmation. Set when an indexed
    // load fetches a would-be entry into `dest`; promoted to a real
    // table ONLY when `dest` is subsequently used as a control-flow
    // target (BX dest / MOV pc,dest / BL into a `bx dest` veneer). This
    // is what distinguishes a jump table from an ordinary indexed load
    // of a DATA pointer (which is dereferenced, not branched to) — the
    // distinction that keeps the detector from seeding garbage.
    struct PendingJt {
        bool     active      = false;
        uint32_t base        = 0;
        uint32_t stride      = 0;
        uint8_t  dest        = 0;
        // Exact entry count from the dispatcher's CMP-bound, when present.
        bool     count_known = false;
        uint32_t count       = 0;
        bool record_slots = false;
        bool finite_sparse = false, signed_index = false;
    };
    PendingJt pend;

    // Finite byte indexes can select callback fields in record arrays. Keep
    // their actual multiply (e.g. (i*2+i)*8), not a guessed word-table stride.
    struct ByteIndex { bool known=false; uint32_t origin=0, scale=0; bool signed_index=false; };
    struct RecordAddress { bool known=false; uint32_t base=0, stride=0; bool signed_index=false; };
    ByteIndex byte_index[16];
    RecordAddress record_address[16];
    // Exact domains of byte-valued arguments. They qualify all 256 possible
    // inputs to a callback selector, not a guessed length of its ROM table.
    struct ByteDomain { bool known=false, signed_value=false; };
    ByteDomain byte_domain[16];

    // A narrow, adjacent load/scale/ADD-pc chain. Unlike abs32 tables these
    // entries are unsigned halfword offsets from the *unaligned* Thumb PC.
    struct PendingRelativeJt {
        bool active = false;
        bool scaled = false;
        uint8_t dest = 0;
        uint32_t base = 0, stride = 0, count = 0, next_pc = 0;
    };
    PendingRelativeJt relative;
    std::vector<uint32_t> switch_targets;
    std::unordered_set<uint32_t> switch_target_set;
    std::size_t next_switch_target = 0;
    uint32_t block_start = pc;
    bool in_switch_block = false;
    uint32_t callback_branch_start=pc;
    bool callback_branch_constant=false;
    std::set<uint32_t> function_callback_branches;

    struct RegSym {
        bool    known = false;
        uint8_t rn = 0;
        int32_t offset = 0;
    };
    RegSym reg_sym[16];

    struct MemConst {
        bool     known = false;
        uint8_t  rn = 0;
        int32_t  offset = 0;
        uint32_t value = 0;
    };
    MemConst mem_const[8];

    auto clear_mem_const = [&]() {
        for (auto& slot : mem_const) {
            slot.known = false;
        }
    };

    auto invalidate_mem_base = [&](uint8_t rn) {
        for (auto& slot : mem_const) {
            if (slot.known && slot.rn == rn) {
                slot.known = false;
            }
        }
    };

    auto invalidate_sym_base = [&](uint8_t rn) {
        for (auto& sym : reg_sym) {
            if (sym.known && sym.rn == rn) {
                sym.known = false;
            }
        }
    };

    auto resolve_symbolic_base = [&](uint8_t base_reg, int32_t offset,
                                     uint8_t* rn,
                                     int32_t* resolved_offset) -> bool {
        if (base_reg >= 15) return false;
        if (reg_sym[base_reg].known) {
            *rn = reg_sym[base_reg].rn;
            *resolved_offset = reg_sym[base_reg].offset + offset;
        } else {
            *rn = base_reg;
            *resolved_offset = offset;
        }
        return true;
    };

    auto eval_symbolic_mem_addr = [&](const armv4t::Instr& i,
                                      uint8_t* rn,
                                      int32_t* offset) -> bool {
        if (i.mem.by_register || i.mem.rn >= 15) return false;
        int32_t mem_offset = 0;
        if (!i.mem.pre_indexed) {
            mem_offset = 0;
        } else {
            int32_t imm = static_cast<int32_t>(i.mem.imm_offset);
            mem_offset = i.mem.add ? imm : -imm;
        }
        return resolve_symbolic_base(i.mem.rn, mem_offset, rn, offset);
    };

    auto remember_mem_const = [&](uint8_t rn, int32_t offset,
                                  uint32_t value) {
        for (auto& slot : mem_const) {
            if (slot.known && slot.rn == rn && slot.offset == offset) {
                slot.value = value;
                return;
            }
        }
        for (auto& slot : mem_const) {
            if (!slot.known) {
                slot.known = true;
                slot.rn = rn;
                slot.offset = offset;
                slot.value = value;
                return;
            }
        }
        mem_const[0].known = true;
        mem_const[0].rn = rn;
        mem_const[0].offset = offset;
        mem_const[0].value = value;
    };

    auto lookup_mem_const = [&](uint8_t rn, int32_t offset,
                                uint32_t* out) -> bool {
        for (const auto& slot : mem_const) {
            if (slot.known && slot.rn == rn && slot.offset == offset) {
                *out = slot.value;
                return true;
            }
        }
        return false;
    };

    auto forget_mem_const = [&](uint8_t rn, int32_t offset) {
        for (auto& slot : mem_const) {
            if (slot.known && slot.rn == rn && slot.offset == offset) {
                slot.known = false;
            }
        }
    };

    auto reg_list_count = [](uint16_t list) -> int {
        int n = 0;
        while (list != 0) {
            n += static_cast<int>(list & 1u);
            list >>= 1;
        }
        return n;
    };

    auto regs_before = [](uint16_t list, int reg) -> int {
        int n = 0;
        for (int r = 0; r < reg; ++r) {
            if (list & (1u << r)) ++n;
        }
        return n;
    };

    auto eval_block_reg_offset = [&](const armv4t::Instr& i, int reg,
                                     bool relative_to_writeback_base,
                                     int32_t* offset) -> bool {
        if (i.block.rn >= 15) return false;
        uint16_t list = i.block.reg_list;
        bool empty_list = (list == 0);
        if (empty_list && reg != 15) return false;
        if (!empty_list && (list & (1u << reg)) == 0) return false;
        int n = empty_list ? 1 : reg_list_count(list);

        int32_t start = 0;
        if (empty_list) {
            if (i.block.add) {
                start = i.block.pre_indexed ? 4 : 0;
            } else {
                start = i.block.pre_indexed ? -0x40 : -0x3c;
            }
        } else if (i.block.add) {
            start = i.block.pre_indexed ? 4 : 0;
        } else {
            start = i.block.pre_indexed ? -4 * n : -4 * (n - 1);
        }
        int32_t final_delta = i.block.add
            ? (empty_list ? 0x40 : 4 * n)
            : (empty_list ? -0x40 : -4 * n);
        int32_t addr_offset = start +
            (empty_list ? 0 : 4 * regs_before(list, reg));
        int32_t base_delta =
            (relative_to_writeback_base && i.block.writeback)
                ? final_delta : 0;
        *offset = addr_offset - base_delta;
        return true;
    };

    auto enqueue_resolved_target = [&](uint32_t raw, CpuMode target_mode,
                                       const char* kind) {
        uint32_t target = raw & ~uint32_t{1};
        const uint32_t target_step =
            (target_mode == CpuMode::Thumb) ? 2u : 4u;
        if (target == 0 || !can_read_at(target, target_step)) return;
        if (addr_in_data_range(target)) {
            record_collision(target, fn.addr, fn.name, kind);
            return;
        }
        if (target_mode == entry_mode) {
            fn.direct_branch_targets.push_back(target);
        } else {
            // Propagate speculation: a cross-mode target reached from a
            // speculative (literal-pool) walk is itself speculative.
            mode_switch_seeds_.push_back(
                FunctionSeed{target, target_mode, "", 0, speculative});
        }
        ++stats_.branch_targets_discovered;
    };

    // Code-vs-data discriminator for a jump-table entry target. Accepts
    // any plausible CODE target (NOT just a function prologue — a real
    // `switch` table points at mid-function case labels that open with
    // arbitrary instructions, e.g. `add r0,r4,#0`), while still rejecting
    // DATA words that merely happen to fall in ROM. The old gate required
    // a `push`/`stmfd` prologue and so rejected every switch table whose
    // cases aren't function entries (this is exactly what blocked
    // MC-HP-001). Checks, in order: correct alignment for the mode; lands
    // in ROM and not in a known data_range; decodes to defined
    // instruction(s) in `mode`. `strict` (used only when no exact count
    // bound exists) decodes a short run and requires every instruction
    // defined — a single `!is_undefined` is too weak to terminate an
    // unbounded walk, since most 0x08xxxxxx words decode to *something*.
    auto looks_like_code = [&](uint32_t tgt, CpuMode mode,
                               bool strict) -> bool {
        if (mode == CpuMode::Thumb) {
            if (tgt & 1u) return false;
        } else if (tgt & 3u) {
            return false;
        }
        if (!can_read_at(tgt, mode == CpuMode::Thumb ? 2u : 4u) ||
            addr_in_data_range(tgt)) return false;
        uint32_t src = 0;
        if (!map_addr_to_source(tgt, &src)) return false;
        const uint32_t k = strict ? 6u : 1u;
        uint32_t a = tgt;
        for (uint32_t j = 0; j < k; ++j) {
            if (mode == CpuMode::Thumb) {
                if (!can_read_at(a, 2)) return j > 0;
                if (armv4t::ThumbDecoder::decode(read_u16(a), a).is_undefined)
                    return false;
                a += 2;
            } else {
                if (!can_read_at(a, 4)) return j > 0;
                if (armv4t::ArmDecoder::decode(read_u32(a), a).is_undefined)
                    return false;
                a += 4;
            }
        }
        return true;
    };

    // Literal-pool harvesting (Stage 0.1, ported from jrickey gba-recomp
    // analyze.rs). A PC-relative `LDR rX,[pc,#imm]` whose loaded word
    // looks like a code pointer seeds a new function entry. This catches
    // function pointers stashed in literal pools that are NEVER consumed
    // by a statically-resolvable branch (stored into a callback table,
    // passed as an argument, returned) — the main source of "missed
    // function" dispatch gaps. SPECULATIVE + SILENT: the heuristic is
    // bit0==1 → THUMB target, (word&3)==0 → ARM target; anything else is
    // not a pointer. looks_like_code (strict) gates on alignment, ROM
    // bounds, data_range, and a defined-instruction decode run. A wrong
    // guess is a dead func_XXXX that is never dispatched, never a hard
    // data_range collision (so it cannot abort the build).
    // ARM function-entry prologue gate. Literal-pool code pointers point
    // at function ENTRIES, and a real ARM function opens with the AAPCS
    // push `STMFD sp!, {..., lr}` (Full Descending: P=1,U=0,W=1,L=0, base
    // SP, LR in the list). Some older/APCS frames lead with `mov ip,sp`
    // before the push, so scan the first two instructions. A ROM *data*
    // word essentially never points at such a push, which is the strong
    // second signal ARM harvesting needs: plain alignment + a 6-instr
    // "all defined" decode is far too weak for ARM (its condition field
    // makes nearly any word decode to something), and without this gate
    // an ARM harvest exploded Minish Cap discovery 7.5x (~292k bogus
    // funcs from data pointers). THUMB needs no such gate — bit0-set +
    // ROM-range + strict decode is already a strong enough signal.
    auto is_arm_func_prologue = [&](uint32_t tgt) -> bool {
        uint32_t a = tgt;
        for (int j = 0; j < 2; ++j) {
            if (!can_read_at(a, 4)) return false;
            armv4t::Instr i = armv4t::ArmDecoder::decode(read_u32(a), a);
            if (i.op == armv4t::IrOp::STM && !i.block.load &&
                i.block.writeback && i.block.rn == 13 &&
                i.block.pre_indexed && !i.block.add &&
                (i.block.reg_list & (1u << 14)) != 0) {
                return true;  // STMFD sp!, {..., lr}  == push {..., lr}
            }
            a += 4;
        }
        return false;
    };
    auto maybe_harvest_literal = [&](uint32_t word) {
        if (!speculative_literal_harvest_enabled_) return;
        ++stats_.literal_pool_words_seen;
        CpuMode tmode;
        uint32_t tgt;
        if (word & 1u) {                 // bit0 set → THUMB pointer
            tmode = CpuMode::Thumb;
            tgt = word & ~uint32_t{1};
        } else if ((word & 3u) == 0u) {  // word-aligned → ARM pointer
            tmode = CpuMode::Arm;
            tgt = word;
        } else {
            return;                      // half-aligned even → not code
        }
        if (!looks_like_code(tgt, tmode, /*strict=*/true)) return;
        // ARM gets the extra entry-prologue gate; THUMB's signal suffices.
        if (tmode == CpuMode::Arm && !is_arm_func_prologue(tgt)) return;
        ++stats_.literal_pool_seeds_kept;
        literal_pool_seeds_.push_back(
            FunctionSeed{tgt, tmode, "", 0, /*speculative=*/true});
    };

    // Recognize an abs32 jump table at `base` revealed by an indexed load
    // at `site_pc`. Mirrors the manual [[jump_table]] expansion in
    // main.cpp (mark table bytes as data + seed each entry) but derives
    // base/count/mode automatically.
    //
    //  * `interworking`  — how the dispatcher consumes an entry. true for
    //    `BX Rt` / a `bx`-veneer reached by BL: bit0 of each word selects
    //    ARM/THUMB (the classic abs32-with-thumb-bit table). false for
    //    `MOV pc,Rt`: a non-interworking write to PC that KEEPS the
    //    dispatcher's current mode, so the words carry no mode bit and
    //    every target inherits `entry_mode`. Deriving mode from bit0 here
    //    (the old behavior) mis-seeded MC-HP-001's even THUMB targets as
    //    ARM.
    //  * `count_hint`    — exact entry count from the dispatcher's
    //    `CMP index,#N; B{hi,cs} default` bound, or 0 if none was
    //    recovered. When known it is the authoritative terminator (no
    //    guessing); a within-range entry that fails the code check then
    //    means the detection was bogus, so the whole table is rejected.
    //    When unknown we fall back to a strict validate-and-stop walk.
    //
    // Overlap with a manual entry is benign: a manual [[jump_table]]
    // already marks the bytes as a data_range, so the walk yields nothing.
    auto emit_jump_table = [&](uint32_t base, uint32_t stride,
                               uint32_t site_pc, bool interworking,
                               uint32_t count_hint) {
        if (stride != 4) return;  // abs32 only in this pass
        // Account each distinct base once, no matter how many overlapping
        // seed walks confirm the same dispatcher.
        if (!jt_seen_bases_.insert(base).second) return;
        const bool bounded = count_hint > 0;
        // Some ARM codecs place a sparse absolute table immediately before
        // their PUSH-LR dispatcher. The known code entry bounds that data
        // span; a null slot is not its terminator. Qualify the entire span
        // before accepting holes, and keep the ordinary strict prefix walk
        // when the layout does not prove a complete null/code-pointer array.
        uint32_t layout_count = 0;
        if (!bounded && entry_mode == CpuMode::Arm && base < fn.addr &&
            ((fn.addr-base) & 3u) == 0 && fn.addr-base <= 4096u &&
            is_arm_func_prologue(fn.addr)) {
            const uint32_t count = (fn.addr-base)/4u;
            uint32_t valid = 0;
            bool complete = true;
            for (uint32_t i=0;i<count;++i) {
                const uint32_t pos=base+i*4u;
                if (!can_read_at(pos,4) || addr_in_data_range(pos)) {
                    complete=false; break;
                }
                const uint32_t raw=read_u32(pos);
                if (!raw) continue;
                const auto mode=interworking
                    ? ((raw & 1u) ? CpuMode::Thumb : CpuMode::Arm) : entry_mode;
                const uint32_t target=raw & (interworking || mode==CpuMode::Thumb ? ~1u : ~3u);
                if (!looks_like_code(target,mode,true)) { complete=false; break; }
                ++valid;
            }
            if (complete && valid>=2u) layout_count=count;
        }
        const uint32_t limit = bounded ? count_hint : layout_count ? layout_count : 1024u;
        std::vector<FunctionSeed> targets;
        std::vector<uint32_t> target_slots;
        // Benign overlap: the table bytes are already covered by a
        // data_range (a manual [[jump_table]] hint, or a prior auto table).
        // The walk yields nothing and we suppress silently — this is the
        // detector independently rediscovering a hand-annotated table, not
        // a failure. Distinguished from a genuine bound mismatch below.
        bool overlap = false;
        for (uint32_t i = 0; i < limit; ++i) {
            uint32_t pos = base + i * stride;
            if (!can_read_at(pos, 4)) break;
            if (addr_in_data_range(pos)) { if (i == 0) overlap = true; break; }
            uint32_t raw = read_u32(pos);
            if (layout_count && !raw) continue;
            CpuMode tmode;
            uint32_t tgt;
            if (interworking) {
                tmode = (raw & 1u) ? CpuMode::Thumb : CpuMode::Arm;
                tgt = raw & ~uint32_t{1};
            } else {
                tmode = entry_mode;  // MOV pc keeps the dispatcher's mode
                tgt = raw & (entry_mode == CpuMode::Thumb ? ~uint32_t{1}
                                                          : ~uint32_t{3});
            }
            if (tgt == 0 || !looks_like_code(tgt, tmode, /*strict=*/!bounded))
                break;
            targets.push_back(FunctionSeed{tgt, tmode, ""});
            target_slots.push_back(i);
        }

        const bool ok = bounded
            ? (targets.size() == count_hint)  // every bounded entry was code
            : (targets.size() >= 2);          // long enough to trust
        if (jt_report_enabled()) {
            std::fprintf(stderr,
                "[jt] base=0x%08X site=0x%08X %-6s bound=%-4s "
                "want=%u got=%zu -> %s\n",
                base, site_pc, interworking ? "BX" : "MOVpc",
                bounded ? "yes" : "no", count_hint, targets.size(),
                ok ? "EMIT" : (overlap ? "overlap" : "reject"));
        }
        if (!ok) {
            if (overlap)       ++stats_.jt_overlap_suppressed;
            else if (bounded)  ++stats_.jt_rejected_bound_mismatch;
            else               ++stats_.jt_rejected_unsized;
            return;
        }

        uint32_t count = layout_count ? layout_count : static_cast<uint32_t>(targets.size());
        data_ranges_.push_back(
            DataRange{base, base + count * stride, "auto jump_table"});
        for (uint32_t i = 0; i < targets.size(); ++i) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "autojt_%08X_%02u", base, target_slots[i]);
            targets[i].name = buf;
            mode_switch_seeds_.push_back(targets[i]);
            ++stats_.auto_jump_table_targets;
        }
        auto_jump_tables_.push_back(
            AutoJumpTable{base, stride, count, site_pc, entry_mode,
                          interworking, bounded || layout_count != 0});
        ++stats_.auto_jump_tables;
    };

    auto eval_dp_imm_pc_write = [&](const armv4t::Instr& i,
                                    uint32_t* out) -> bool {
        if (i.rd != 15 || i.op2.kind != armv4t::Op2::Kind::Imm) {
            return false;
        }

        switch (i.op) {
            case armv4t::IrOp::MOV:
                *out = i.op2.imm_value;
                return true;
            case armv4t::IrOp::MVN:
                *out = ~i.op2.imm_value;
                return true;
            default:
                break;
        }

        uint32_t lhs = 0;
        if (i.rn == 15) {
            lhs = i.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
        } else if (i.rn < 16 && reg_const[i.rn].known) {
            lhs = reg_const[i.rn].value;
        } else {
            return false;
        }

        uint32_t rhs = i.op2.imm_value;
        switch (i.op) {
            case armv4t::IrOp::ADD:
                *out = lhs + rhs;
                return true;
            case armv4t::IrOp::SUB:
                *out = lhs - rhs;
                return true;
            case armv4t::IrOp::RSB:
                *out = rhs - lhs;
                return true;
            case armv4t::IrOp::AND:
                *out = lhs & rhs;
                return true;
            case armv4t::IrOp::EOR:
                *out = lhs ^ rhs;
                return true;
            case armv4t::IrOp::ORR:
                *out = lhs | rhs;
                return true;
            case armv4t::IrOp::BIC:
                *out = lhs & ~rhs;
                return true;
            default:
                // ADC/SBC/RSC depend on CPSR.C; leave opaque.
                return false;
        }
    };

    auto eval_dp_reg_pc_write = [&](const armv4t::Instr& i,
                                    uint32_t* out) -> bool {
        if (i.rd != 15 ||
            i.op2.kind != armv4t::Op2::Kind::Shifted ||
            i.op2.shifted.by_register ||
            i.op2.shifted.imm_or_rs != 0 ||
            i.op2.shifted.rm >= 16 ||
            !reg_const[i.op2.shifted.rm].known) {
            return false;
        }

        uint32_t rhs = reg_const[i.op2.shifted.rm].value;
        switch (i.op) {
            case armv4t::IrOp::MOV:
                *out = rhs;
                return true;
            case armv4t::IrOp::MVN:
                *out = ~rhs;
                return true;
            default:
                break;
        }

        uint32_t lhs = 0;
        if (i.rn == 15) {
            lhs = i.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
        } else if (i.rn < 16 && reg_const[i.rn].known) {
            lhs = reg_const[i.rn].value;
        } else {
            return false;
        }

        switch (i.op) {
            case armv4t::IrOp::ADD:
                *out = lhs + rhs;
                return true;
            case armv4t::IrOp::SUB:
                *out = lhs - rhs;
                return true;
            case armv4t::IrOp::RSB:
                *out = rhs - lhs;
                return true;
            case armv4t::IrOp::AND:
                *out = lhs & rhs;
                return true;
            case armv4t::IrOp::EOR:
                *out = lhs ^ rhs;
                return true;
            case armv4t::IrOp::ORR:
                *out = lhs | rhs;
                return true;
            case armv4t::IrOp::BIC:
                *out = lhs & ~rhs;
                return true;
            default:
                return false;
        }
    };

    auto eval_mem_imm_addr = [&](const armv4t::Instr& i,
                                 uint32_t* out) -> bool {
        if (i.mem.by_register) return false;

        uint32_t base = 0;
        if (i.mem.rn == 15) {
            base = i.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
            if (entry_mode == CpuMode::Thumb) base &= ~uint32_t{3};
        } else if (i.mem.rn < 16 && reg_const[i.mem.rn].known) {
            base = reg_const[i.mem.rn].value;
        } else {
            return false;
        }

        uint32_t indexed = i.mem.add
            ? (base + i.mem.imm_offset)
            : (base - i.mem.imm_offset);
        *out = i.mem.pre_indexed ? indexed : base;
        return true;
    };

    auto emit_thumb_relative_table = [&](uint32_t base, uint32_t stride,
                                         uint32_t entries, uint32_t site,
                                         bool manual) {
        if ((stride != 1 && stride != 2) || entries == 0 ||
            (stride == 2 && (base & 1u)) ||
            entries > rom_size_ / stride ||
            static_cast<uint64_t>(base) + entries * stride > 0xFFFFFFFFull ||
            !can_read_at(base, entries * stride)) return;
        const uint32_t end = base + entries * stride;
        std::vector<uint32_t> targets;
        for (uint32_t i = 0; i < entries; ++i) {
            uint32_t src = 0;
            if (!map_addr_to_source(base + i * stride, &src)) return;
            const std::size_t off = src - rom_base_;
            uint32_t value = rom_[off];
            if (stride == 2) value |= static_cast<uint32_t>(rom_[off + 1]) << 8;
            const uint64_t raw = static_cast<uint64_t>(site) + 4u + 2u * value;
            if (raw > 0xFFFFFFFFull) return;
            const uint32_t target = static_cast<uint32_t>(raw);
            // Validate the entire bounded table before committing any state.
            if ((target >= base && target < end) ||
                !looks_like_code(target, CpuMode::Thumb, false)) return;
            targets.push_back(target);
        }
        if (!manual && jt_seen_bases_.insert(base).second) {
            add_data_range(base, end, "auto Thumb relative jump_table");
            auto_jump_tables_.push_back(
                AutoJumpTable{base, stride, entries, site, CpuMode::Thumb,
                              false, true});
            ++stats_.auto_jump_tables;
            stats_.auto_jump_table_targets += entries;
        }
        for (uint32_t target : targets) {
            if (switch_target_set.insert(target).second) {
                switch_targets.push_back(target);
                fn.alias_entries.push_back(target);
            }
        }
    };

    auto next_switch_block = [&]() -> bool {
        if (in_switch_block && pc > block_start)
            fn.switch_blocks.push_back({block_start, pc});
        while (next_switch_target < switch_targets.size()) {
            const uint32_t target = switch_targets[next_switch_target++];
            bool covered = target >= entry_addr && target < fn.end_addr;
            for (const auto& block : fn.switch_blocks)
                covered |= target >= block.start && target < block.end;
            if (covered) continue;
            pc = block_start = target;
            in_switch_block = true;
            callback_branch_start=pc;
            callback_branch_constant=false;
            count = 0;  // retain the normal instruction limit per linear walk
            // Each case is a separate CFG entry. No constants, bounds, or
            // pending loads from the dispatcher dominate an arbitrary case.
            for (unsigned r = 0; r < 16; ++r) {
                reg_const[r] = {};
                reg_scaled[r] = {};
                reg_table[r] = {};
                reg_bound[r] = {};
                reg_sym[r] = {};
                byte_index[r] = {};
                record_address[r] = {};
                byte_domain[r] = {};
            }
            clear_mem_const();
            last_cmp = {};
            pend = {};
            relative = {};
            ++relative_epoch;
            return true;
        }
        return false;
    };

    while (true) {
        if (count++ >= kMaxInstrs) {
            if (next_switch_block()) continue;
            break;
        }
        if (!can_read_walk_at(pc, step)) {
            if (next_switch_block()) continue;
            break;
        }
        // If we ever decode from inside a data_range, that's a hard
        // error — TOML asserted "no code here" but our walk got here
        // anyway. Recorded; finder continues so multiple collisions
        // surface at once.
        if (addr_in_data_range(pc)) {
            record_collision(pc, fn.addr, fn.name, "branch");
            break;
        }

        armv4t::Instr ins;
        if (entry_mode == CpuMode::Thumb) {
            ins = armv4t::ThumbDecoder::decode(read_walk_u16(pc), pc);
        } else {
            ins = armv4t::ArmDecoder::decode(read_walk_u32(pc), pc);
        }

        if (ins.is_undefined) {
            ++stats_.undefined_instr_count;
            if (next_switch_block()) continue;
            break;
        }

        if(entry_mode==CpuMode::Thumb && entry_addr==entry_source && addr_in_rom(pc)) {
            if(ins.op==armv4t::IrOp::MOV && ins.rd<13 &&
               ins.op2.kind==armv4t::Op2::Kind::Imm) callback_branch_constant=true;
            if(ins.op==armv4t::IrOp::B && ins.cond==armv4t::Cond::AL &&
               ins.branch_target>pc && callback_branch_constant &&
               pc>=callback_branch_start && pc-callback_branch_start<=64u) {
                // Re-evaluate this predecessor's own instructions with every
                // incoming register unknown. Constants may survive the join in
                // this path only; none are assumed for other predecessors.
                function_callback_branches.insert(callback_branch_start);
            }
        }

        if (entry_mode == CpuMode::Thumb && ins.op == armv4t::IrOp::BL_prefix &&
            can_read_walk_at(pc+2u, 2)) {
            const auto low = armv4t::ThumbDecoder::decode(read_walk_u16(pc+2u), pc+2u);
            if (low.op == armv4t::IrOp::BL_suffix) {
                std::array<uint32_t, 4> arguments{};
                uint8_t known = 0;
                for (unsigned r = 0; r < 4; ++r) if (reg_const[r].known) {
                    known |= uint8_t(1u << r);
                    arguments[r] = reg_const[r].value;
                }
                harvest_contextual_callbacks((ins.branch_target+low.swi_imm)&~1u,
                                              known, arguments);
                for(unsigned r=0;r<4;++r) if(!(known & (1u<<r)) && byte_domain[r].known) {
                    auto finite_arguments=arguments;
                    for(unsigned byte=0;byte<256u;++byte) {
                        finite_arguments[r]=byte_domain[r].signed_value && byte>=128u
                            ? byte|0xFFFFFF00u : byte;
                        harvest_contextual_callbacks((ins.branch_target+low.swi_imm)&~1u,
                            uint8_t(known | (1u<<r)),finite_arguments,0,false,CallbackFlow::ByteDomain);
                    }
                }
            }
        }

        if (entry_mode == CpuMode::Thumb && entry_addr == entry_source &&
            addr_in_rom(pc)) {
            if (ins.op == armv4t::IrOp::B && ins.cond != armv4t::Cond::AL &&
                ins.branch_target < pc) {
                for (const auto& saved : copy_loop_entries) {
                    if (saved.pc == ins.branch_target) {
                        harvest_callback_copy(saved.pc, pc, saved.regs);
                        break;
                    }
                }
            }
            if (ins.is_branch || ins.is_call) copy_loop_entries.clear();
            else {
                CopyLoopEntry saved{pc, {}};
                std::copy(std::begin(reg_const), std::end(reg_const), saved.regs.begin());
                copy_loop_entries.push_back(saved);
                if (copy_loop_entries.size() > 32) copy_loop_entries.pop_front();
            }
        }

        if (entry_mode == CpuMode::Thumb) {
            const auto& operand = ins.op2;
            const bool add_pc = ins.op == armv4t::IrOp::ADD &&
                ins.rd == 15 && ins.rn == 15 &&
                operand.kind == armv4t::Op2::Kind::Shifted &&
                operand.shifted.type == armv4t::ShiftType::LSL &&
                !operand.shifted.by_register && operand.shifted.imm_or_rs == 0;
            if (add_pc) {
                bool manual = false;
                for (const auto& table : thumb_relative_tables_) {
                    if (table.target_base == pc + 4u) {
                        emit_thumb_relative_table(table.addr, table.stride,
                                                  table.count, pc, true);
                        manual = true;
                    }
                }
                if (!manual && jump_table_detection_enabled() &&
                    relative.active && relative.scaled &&
                    relative.next_pc == pc && operand.shifted.rm == relative.dest) {
                    ++stats_.jt_confirmations;
                    emit_thumb_relative_table(relative.base, relative.stride,
                                              relative.count, pc, false);
                }
            }
            if (relative.active) {
                if (pc == relative.next_pc && !relative.scaled &&
                    ins.op == armv4t::IrOp::MOV && ins.rd == relative.dest &&
                    operand.kind == armv4t::Op2::Kind::Shifted &&
                    operand.shifted.rm == relative.dest &&
                    operand.shifted.type == armv4t::ShiftType::LSL &&
                    !operand.shifted.by_register && operand.shifted.imm_or_rs == 1) {
                    relative.scaled = true;
                    relative.next_pc = pc + 2u;
                } else {
                    relative.active = false;
                }
            }
            if ((ins.op == armv4t::IrOp::LDRB || ins.op == armv4t::IrOp::LDRH) &&
                ins.mem.by_register && ins.mem.pre_indexed && ins.mem.add &&
                !ins.mem.writeback && ins.mem.rn < 15 && ins.rd < 15 &&
                ins.mem.reg_offset.rm < 15 &&
                !ins.mem.reg_offset.by_register &&
                ins.mem.reg_offset.type == armv4t::ShiftType::LSL &&
                ins.mem.reg_offset.imm_or_rs == 0 && reg_const[ins.mem.rn].known &&
                relative_base_epoch[ins.mem.rn] == relative_epoch) {
                const uint8_t index = ins.mem.reg_offset.rm;
                const uint32_t stride = ins.op == armv4t::IrOp::LDRB ? 1u : 2u;
                bool bounded = reg_bound[index].known &&
                    relative_bound_epoch[index] == relative_epoch;
                uint32_t maxi = reg_bound[index].max_index;
                // Thumb LDRH has an unscaled register offset. GCC scales
                // the bounded logical index into a byte offset beforehand.
                if (stride == 2) {
                    bounded = reg_scaled[index].known &&
                        reg_scaled[index].shift == 1 && reg_scaled[index].bound_known &&
                        reg_scaled[index].relative_bound_epoch == relative_epoch;
                    maxi = reg_scaled[index].max_index;
                    const auto source = reg_scaled[index].src_reg;
                    if (!bounded && reg_scaled[index].known &&
                        reg_scaled[index].shift == 1 &&
                        reg_scaled[index].relative_source_live && source < 16 &&
                        reg_bound[source].known &&
                        relative_bound_epoch[source] == relative_epoch) {
                        bounded = true;
                        maxi = reg_bound[source].max_index;
                    }
                }
                if (bounded && maxi < 0xFFFFFFFFu) {
                    relative = {true, false, ins.rd, reg_const[ins.mem.rn].value,
                                stride, maxi + 1u, pc + 2u};
                }
            }
        }

        // Record direct branch targets. The decoder fills
        // branch_target with the absolute guest address.
        const bool thumb_bl_half =
            entry_mode == CpuMode::Thumb &&
            (ins.op == armv4t::IrOp::BL_prefix ||
             ins.op == armv4t::IrOp::BL_suffix);
        if (ins.is_branch && !ins.is_indirect &&
            ins.branch_target != 0 && !thumb_bl_half) {
            if (addr_in_data_range(ins.branch_target)) {
                record_collision(ins.branch_target, fn.addr,
                                  fn.name, "branch");
                // Don't push the target as a target; the collision
                // is already recorded. Drop it to avoid a worklist
                // entry that would re-trip the same error.
            } else {
                fn.direct_branch_targets.push_back(ins.branch_target);
                ++stats_.branch_targets_discovered;
            }
        }

        // THUMB BL pair: the decoder splits BL into BL_prefix
        // (upper halfword) + BL_suffix (lower halfword). Neither
        // half alone has the FULL target — prefix has
        // branch_target = (PC+4)+(upper<<12) which is just an
        // intermediate base, suffix has branch_target = 0 plus
        // swi_imm = (imm11<<1). Combining them gives the real
        // target. We do the combine here in the finder so the
        // worklist gets the right address.
        if (entry_mode == CpuMode::Thumb &&
            ins.op == armv4t::IrOp::BL_prefix &&
            can_read_walk_at(pc + 2, 2)) {
            armv4t::Instr lower = armv4t::ThumbDecoder::decode(
                read_walk_u16(pc + 2), pc + 2);
            if (lower.op == armv4t::IrOp::BL_suffix) {
                uint32_t full = ins.branch_target + lower.swi_imm;
                full &= ~uint32_t{1};  // strip THUMB bit if encoded
                if (full != 0 && can_read_at(full, step)) {
                    if (addr_in_data_range(full)) {
                        record_collision(full, fn.addr, fn.name,
                                          "branch");
                    } else {
                        fn.direct_branch_targets.push_back(full);
                        ++stats_.branch_targets_discovered;
                    }
                }

                // The instruction after a THUMB BL pair is always a
                // valid continuation address. Some real BIOS paths
                // and gba-suite cases later reach that continuation
                // through an indirect transfer (saved LR, empty-list
                // LDM, callback trampoline). Seed the continuation
                // explicitly instead of relying on the BL_prefix
                // half's intermediate target, which is not the call
                // target and can point into data for long BLs.
                uint32_t return_pc = pc + 4u;
                if (can_read_at(return_pc, 2)) {
                    enqueue_resolved_target(return_pc, entry_mode, "branch");
                }
            }
        }

        // Indirect = computed; we can't follow statically — UNLESS
        // our narrow constant tracker resolved the source register
        // to a known value just above the BX (or MOV PC, Rn).
        if (ins.op == armv4t::IrOp::SWI) {
            enqueue_resolved_target(ins.pc + step, entry_mode, "branch");
        }

        if (ins.is_indirect) {
            fn.has_indirect_transfer = true;
            ++stats_.indirect_transfer_count;

            // Some PC-writing instructions look "indirect" to the
            // decoder but still have a fully-known target. Keep this
            // intentionally narrow so misses stay loud and false
            // positives stay rare.
            uint32_t resolved = 0;
            if (eval_dp_imm_pc_write(ins, &resolved)) {
                CpuMode target_mode = entry_mode;
                if (ins.rn != 15 && (resolved & 1u)) {
                    target_mode = CpuMode::Thumb;
                }
                enqueue_resolved_target(resolved, target_mode, "branch");
            } else if (eval_dp_reg_pc_write(ins, &resolved)) {
                enqueue_resolved_target(resolved, entry_mode, "branch");
            } else if (ins.op == armv4t::IrOp::LDR && ins.rd == 15) {
                uint32_t ea = 0;
                if (eval_mem_imm_addr(ins, &ea) && can_read_at(ea, 4)) {
                    uint32_t raw = read_u32(ea & ~uint32_t{3});
                    raw = ror32(raw, (ea & 3u) * 8u);
                    CpuMode target_mode = (raw & 1u)
                        ? CpuMode::Thumb : entry_mode;
                    enqueue_resolved_target(raw, target_mode, "branch");
                } else {
                    uint8_t rn = 0;
                    int32_t offset = 0;
                    uint32_t raw = 0;
                    if (eval_symbolic_mem_addr(ins, &rn, &offset) &&
                        lookup_mem_const(rn, offset, &raw)) {
                        CpuMode target_mode = (raw & 1u)
                            ? CpuMode::Thumb : entry_mode;
                        enqueue_resolved_target(raw, target_mode, "branch");
                    }
                }
            } else if (ins.op == armv4t::IrOp::LDM &&
                       ins.block.load &&
                       ((ins.block.reg_list & (1u << 15)) != 0 ||
                        ins.block.reg_list == 0)) {
                int32_t offset = 0;
                uint8_t rn = 0;
                int32_t resolved_offset = 0;
                uint32_t raw = 0;
                if (eval_block_reg_offset(
                        ins, 15, /*relative_to_writeback_base=*/false,
                        &offset) &&
                    resolve_symbolic_base(ins.block.rn, offset,
                                          &rn, &resolved_offset) &&
                    lookup_mem_const(rn, resolved_offset, &raw)) {
                    enqueue_resolved_target(raw, entry_mode, "branch");
                }
            }

            // Two cases land here with a tracked register source:
            //   1. BX Rm                  — `ins.rm` is the source
            //   2. MOV PC, Rn (DP-MOV)    — `ins.op2.shifted.rm`
            //      is the source (op2 with no shift)
            // BX is the ARMv4T interworking instruction, so it gets
            // target mode from bit 0. Plain DP writes to PC keep the
            // current instruction-set state; bit 0 is not an exchange.
            // DP writes to PC are already handled by the evaluators above.
            // Resolve only BX here: using its operand as the final target is
            // valid, while doing that for ADD/EOR/etc. invents code edges.
            if (ins.op == armv4t::IrOp::BX && ins.rm < 16) {
                uint32_t raw = 0;
                bool raw_known = false;
                if (ins.rm == 15) {
                    raw = ins.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
                    if (entry_mode == CpuMode::Thumb) raw &= ~uint32_t{3};
                    raw_known = true;
                } else if (reg_const[ins.rm].known) {
                    raw = reg_const[ins.rm].value;
                    raw_known = true;
                }
                if (raw_known) {
                    CpuMode tgt_mode = (raw & 1u)
                        ? CpuMode::Thumb : CpuMode::Arm;
                    enqueue_resolved_target(raw, tgt_mode, "branch");
                }
            }
            if (ins.op == armv4t::IrOp::BX &&
                ins.rm != 14 &&
                reg_const[14].known) {
                uint32_t raw = reg_const[14].value;
                CpuMode return_mode = (raw & 1u)
                    ? CpuMode::Thumb : CpuMode::Arm;
                enqueue_resolved_target(raw, return_mode, "branch");
            }
        }

        // Base-relative virtual callback: LDR offset,[base,#slot]; ADD target,
        // base,offset; BL <BX-target veneer>. Capture the explicit slot only,
        // including when the object supplies an unknown vtable base at runtime.
        if (entry_mode == CpuMode::Thumb && aot_scan_start_ < aot_scan_end_ &&
            ins.op == armv4t::IrOp::LDR && !ins.mem.by_register &&
            ins.mem.pre_indexed && ins.mem.add && !ins.mem.writeback &&
            ins.mem.rn < 15 && ins.rd < 15 && ins.rd != ins.mem.rn &&
            ins.mem.imm_offset <= 256u && (ins.mem.imm_offset & 3u)==0 &&
            can_read_walk_at(pc+2,6)) {
            const auto add=armv4t::ThumbDecoder::decode(read_walk_u16(pc+2),pc+2);
            const auto hi=armv4t::ThumbDecoder::decode(read_walk_u16(pc+4),pc+4);
            const auto lo=armv4t::ThumbDecoder::decode(read_walk_u16(pc+6),pc+6);
            const auto& o=add.op2;
            if (add.op==armv4t::IrOp::ADD && add.rd<15 &&
                o.kind==armv4t::Op2::Kind::Shifted && !o.shifted.by_register &&
                o.shifted.type==armv4t::ShiftType::LSL && o.shifted.imm_or_rs==0 &&
                ((add.rn==ins.mem.rn && o.shifted.rm==ins.rd) ||
                 (add.rn==ins.rd && o.shifted.rm==ins.mem.rn)) &&
                hi.op==armv4t::IrOp::BL_prefix && lo.op==armv4t::IrOp::BL_suffix) {
                const uint32_t target=(hi.branch_target+lo.swi_imm)&~1u;
                if (can_read_at(target,2)) {
                    const auto veneer=armv4t::ThumbDecoder::decode(read_u16(target),target);
                    if (veneer.op==armv4t::IrOp::BX && veneer.rm==add.rd &&
                        relative_callback_slots_.insert(ins.mem.imm_offset).second)
                        relative_callbacks_dirty_=true;
                }
            }
        }

        // A copied codec can initialize instruction slots with an exact
        // compare-and-replace loop. Keep only a proven same-address EQ store
        // of a constant ARM STR instruction, rooted in ROM-backed RAM.
        if (entry_mode==CpuMode::Arm && ins.op==armv4t::IrOp::LDR &&
            ins.cond==armv4t::Cond::AL && ins.rd<15 && ins.mem.rn<15 &&
            ins.mem.by_register && ins.mem.pre_indexed && ins.mem.add &&
            ins.rd!=ins.mem.rn && ins.rd!=ins.mem.reg_offset.rm &&
            !ins.mem.writeback && !ins.mem.reg_offset.by_register &&
            ins.mem.reg_offset.type==armv4t::ShiftType::LSL &&
            ins.mem.reg_offset.imm_or_rs==2u &&
            reg_const[ins.mem.rn].known && can_read_walk_at(pc+4u,8u)) {
            const uint32_t base=reg_const[ins.mem.rn].value;
            const auto cmp=armv4t::ArmDecoder::decode(read_walk_u32(pc+4u),pc+4u);
            const auto st=armv4t::ArmDecoder::decode(read_walk_u32(pc+8u),pc+8u);
            const auto& co=cmp.op2;
            const auto& sm=st.mem;
            if ((base>>24==2u || base>>24==3u) && can_read_at(base,4u) &&
                cmp.op==armv4t::IrOp::CMP && cmp.cond==armv4t::Cond::AL &&
                cmp.rn==ins.rd && co.kind==armv4t::Op2::Kind::Shifted &&
                !co.shifted.by_register && co.shifted.type==armv4t::ShiftType::LSL &&
                co.shifted.imm_or_rs==0 && co.shifted.rm<15 && co.shifted.rm!=ins.rd &&
                reg_const[co.shifted.rm].known && st.op==armv4t::IrOp::STR &&
                st.cond==armv4t::Cond::EQ && st.rd<15 && st.rd!=ins.rd &&
                reg_const[st.rd].known && sm.rn==ins.mem.rn && sm.by_register &&
                sm.pre_indexed && sm.add && !sm.writeback &&
                sm.reg_offset.rm==ins.mem.reg_offset.rm &&
                sm.reg_offset.type==armv4t::ShiftType::LSL &&
                !sm.reg_offset.by_register && sm.reg_offset.imm_or_rs==2u) {
                const uint32_t replacement=reg_const[st.rd].value;
                const auto op=armv4t::ArmDecoder::decode(replacement,base);
                if (!op.is_undefined && op.cond==armv4t::Cond::AL &&
                    op.op==armv4t::IrOp::STR && !op.mem.by_register &&
                    op.mem.rn<15 && op.rd<15)
                    constant_code_patches_.push_back({base,reg_const[co.shifted.rm].value,replacement});
            }
        }

        // Jump-table confirmation (MC-HP-000). If a candidate from a
        // prior indexed load is pending, check whether THIS instruction
        // uses the loaded register as a control-flow target. That
        // promotion is the proof it's a jump table rather than an
        // indexed DATA-pointer load (which is dereferenced, not branched
        // to). Without this gate the detector seeds garbage and the walk
        // explodes.
        if (pend.active) {
            bool confirmed = false;
            // How the dispatcher consumes the loaded entry decides target
            // mode: BX / bx-veneer interwork on bit0; MOV pc keeps the
            // dispatcher's current mode. See emit_jump_table().
            bool interworking = true;
            const armv4t::Op2& po = ins.op2;
            if (ins.op == armv4t::IrOp::BX && ins.rm == pend.dest) {
                confirmed = true;  // BX dest
                interworking = true;
            } else if (ins.op == armv4t::IrOp::MOV && ins.rd == 15 &&
                       po.kind == armv4t::Op2::Kind::Shifted &&
                       !po.shifted.by_register &&
                       po.shifted.imm_or_rs == 0 &&
                       po.shifted.rm == pend.dest) {
                confirmed = true;  // MOV pc, dest (computed jump)
                interworking = false;
            } else if (ins.op == armv4t::IrOp::BL && ins.branch_target != 0 &&
                       can_read_at(ins.branch_target, 2)) {
                // ARM BL into a `bx dest` veneer (single-instruction BL).
                armv4t::Instr v = armv4t::ThumbDecoder::decode(
                    read_u16(ins.branch_target), ins.branch_target);
                if (v.op == armv4t::IrOp::BX && v.rm == pend.dest) {
                    confirmed = true;
                }
            } else if (entry_mode == CpuMode::Thumb &&
                       ins.op == armv4t::IrOp::BL_prefix &&
                       can_read_walk_at(ins.pc + 2, 2)) {
                // THUMB BL is a prefix/suffix halfword PAIR; the full
                // target only exists once combined. This is the dominant
                // dispatch form — `bl <bx-dest veneer>` — so without it
                // every THUMB-BL-dispatched table is missed. Combine the
                // pair (same math as the finder's direct-BL handling),
                // then check the veneer it lands on.
                armv4t::Instr lo = armv4t::ThumbDecoder::decode(
                    read_walk_u16(ins.pc + 2), ins.pc + 2);
                if (lo.op == armv4t::IrOp::BL_suffix) {
                    uint32_t full =
                        (ins.branch_target + lo.swi_imm) & ~uint32_t{1};
                    if (can_read_at(full, 2)) {
                        armv4t::Instr v = armv4t::ThumbDecoder::decode(
                            read_u16(full), full);
                        if (v.op == armv4t::IrOp::BX &&
                            v.rm == pend.dest) {
                            confirmed = true;
                        }
                    }
                }
            }
            if (confirmed) {
                ++stats_.jt_confirmations;
                if (pend.record_slots || pend.finite_sparse) {
                    // Byte loads prove all 256 possible indexes. Null/payload slots
                    // are skipped, never used as a heuristic table terminator.
                    // Require an interworking consumer and strong code entries;
                    // other record fields and their extents remain unclassified.
                    if (interworking) for (uint32_t n=0;n<256u;++n) {
                        const int64_t index=pend.signed_index && n>=128u ? int64_t(n)-256 : int64_t(n);
                        const int64_t slot=int64_t(pend.base)+index*pend.stride;
                        if (slot<0 || slot>UINT32_MAX || !can_read_at(uint32_t(slot),4)) continue;
                        const uint32_t raw=read_u32(uint32_t(slot)),target=raw & ~1u;
                        if(pend.finite_sparse && (target<aot_scan_start_ ||
                            uint64_t(target)+12u>aot_scan_end_)) continue;
                        const CpuMode mode=(raw & 1u) ? CpuMode::Thumb : CpuMode::Arm;
                        const bool prologue=mode==CpuMode::Thumb
                            ? is_thumb_func_prologue(target) : is_arm_func_prologue(target);
                        if (prologue && looks_like_code(target,mode,true) &&
                            aot_seen_seed_keys_.insert(visit_key(target,mode)).second) {
                            aot_pointer_seeds_.push_back({target,mode,"",0,true});
                            ++stats_.aot_callback_seeds;
                        }
                    }
                }
                if (!pend.record_slots) {
                    // Preserve the existing word-switch discovery and sizing;
                    // finite sparse callback harvesting is additional evidence.
                    emit_jump_table(pend.base, pend.stride, ins.pc, interworking,
                                    pend.count_known ? pend.count : 0u);
                }
                pend.active = false;
            } else if (may_clobber_register(ins, pend.dest)) {
                pend.active = false;  // dest overwritten before any branch
            }
        }

        // Jump-table detection. An indexed load of a would-be table entry
        // whose address is (tracked ROM-pointer base) + (index<<2). Two
        // shapes:
        //   (a) register-offset:  LDR Rt,[Rb, Ri, LSL #2]
        //   (b) the common THUMB form where a prior ADD folded
        //       base + (index<<2) into reg_table[] (see maintenance below).
        // Records a pending candidate; emission waits for the
        // confirmation above. Detection runs before the tracker update so
        // reg_table[] from the preceding ADD is still live even when
        // Rt == the base reg.
        if (jump_table_detection_enabled() && ins.op == armv4t::IrOp::LDR) {
            uint32_t jt_base = 0;
            bool jt = false;
            bool     count_known = false;
            uint32_t count = 0;
            if (ins.mem.by_register && ins.mem.rn < 16 &&
                ins.mem.reg_offset.rm < 16 &&
                ins.mem.reg_offset.type == armv4t::ShiftType::LSL &&
                !ins.mem.reg_offset.by_register &&
                (ins.mem.reg_offset.imm_or_rs == 2 ||
                 ins.mem.reg_offset.imm_or_rs == 0) &&
                reg_const[ins.mem.rn].known &&
                can_read_at(reg_const[ins.mem.rn].value,4)) {
                jt_base = reg_const[ins.mem.rn].value;
                jt = true;
                // Inline-scaled form: the index is the offset register.
                uint8_t ireg = ins.mem.reg_offset.rm;
                if (ins.mem.reg_offset.imm_or_rs == 2 && reg_bound[ireg].known) {
                    count_known = true;
                    count = reg_bound[ireg].max_index + 1u;
                }
            } else if (!ins.mem.by_register && ins.mem.rn < 16 &&
                       reg_table[ins.mem.rn].known &&
                       (reg_table[ins.mem.rn].stride == 4 ||
                        reg_table[ins.mem.rn].stride == 0)) {
                int32_t off = 0;
                if (ins.mem.pre_indexed) {
                    int32_t imm = static_cast<int32_t>(ins.mem.imm_offset);
                    off = ins.mem.add ? imm : -imm;
                }
                jt_base = reg_table[ins.mem.rn].base +
                          static_cast<uint32_t>(off);
                jt = true;
                if (reg_table[ins.mem.rn].stride == 4 && reg_table[ins.mem.rn].bound_known) {
                    count_known = true;
                    count = reg_table[ins.mem.rn].max_index + 1u;
                }
            }
            if (jt) {
                pend.active      = true;
                pend.base        = jt_base;
                pend.stride      = 4;
                pend.dest        = ins.rd;
                pend.count_known = count_known;
                pend.count       = count;
                pend.record_slots = false;
                pend.finite_sparse = false;pend.signed_index=false;
            }
        }

        if (jump_table_detection_enabled() && ins.op==armv4t::IrOp::LDR &&
            !ins.mem.by_register && !ins.mem.writeback && ins.mem.rn<15 &&
            ins.rd<15 && record_address[ins.mem.rn].known) {
            const auto& address=record_address[ins.mem.rn];
            const uint32_t base=address.base+(ins.mem.pre_indexed
                ? (ins.mem.add ? ins.mem.imm_offset : -ins.mem.imm_offset) : 0u);
            if ((base & 3u)==0 && address.stride>4u && (address.stride & 3u)==0) {
                pend.active=true; pend.base=base; pend.stride=address.stride;
                pend.dest=ins.rd; pend.record_slots=true;
                pend.finite_sparse=false;pend.signed_index=address.signed_index;
            } else if(aot_scan_start_<aot_scan_end_ && (base & 3u)==0 && address.stride==4u &&
                      pend.active && pend.base==base && !pend.count_known) {
                // Additional sparse callback admission requires an explicit
                // executable span; every candidate target stays inside it.
                pend.finite_sparse=true;pend.signed_index=address.signed_index;
            }
        }

        ByteIndex next_byte{};
        RecordAddress next_record{};
        ByteDomain next_domain{};
        const auto& record_op=ins.op2;
        if(ins.op==armv4t::IrOp::LDRB || ins.op==armv4t::IrOp::LDRSB)
            next_domain={true,ins.op==armv4t::IrOp::LDRSB};
        else if(ins.op==armv4t::IrOp::MOV && record_op.kind==armv4t::Op2::Kind::Shifted &&
                !record_op.shifted.by_register && record_op.shifted.rm<16) {
            const auto& shift=record_op.shifted;
            if(shift.type==armv4t::ShiftType::LSL && shift.imm_or_rs==0)
                next_domain=byte_domain[shift.rm];
            else if(shift.imm_or_rs==24 &&
                (shift.type==armv4t::ShiftType::LSR || shift.type==armv4t::ShiftType::ASR))
                next_domain={true,shift.type==armv4t::ShiftType::ASR};
        }
        if ((ins.op==armv4t::IrOp::LDRB ||
             (ins.op==armv4t::IrOp::LDRSB && aot_scan_start_<aot_scan_end_)) && ins.rd<15) {
            next_byte={true,pc,1u,ins.op==armv4t::IrOp::LDRSB};
        } else if (record_op.kind==armv4t::Op2::Kind::Shifted &&
                   !record_op.shifted.by_register &&
                   record_op.shifted.type==armv4t::ShiftType::LSL &&
                   record_op.shifted.rm<15 && record_op.shifted.imm_or_rs<24u) {
            const uint8_t rm=record_op.shifted.rm;
            const uint32_t shift=record_op.shifted.imm_or_rs;
            const uint64_t scale=uint64_t(byte_index[rm].scale)<<shift;
            if (ins.op==armv4t::IrOp::MOV && byte_index[rm].known &&
                scale*255u<=UINT32_MAX) {
                next_byte={true,byte_index[rm].origin,uint32_t(scale),byte_index[rm].signed_index};
            } else if (ins.op==armv4t::IrOp::ADD && ins.rn<15) {
                const auto& lhs=byte_index[ins.rn];
                if (lhs.known && byte_index[rm].known &&
                    lhs.origin==byte_index[rm].origin && lhs.signed_index==byte_index[rm].signed_index &&
                    (scale+lhs.scale)*255u<=UINT32_MAX)
                    next_byte={true,lhs.origin,uint32_t(scale+lhs.scale),lhs.signed_index};
                else if (reg_const[ins.rn].known && byte_index[rm].known &&
                         addr_in_rom(reg_const[ins.rn].value) && scale*255u<=UINT32_MAX)
                    next_record={true,reg_const[ins.rn].value,uint32_t(scale),byte_index[rm].signed_index};
                else if (shift==0 && reg_const[rm].known && lhs.known &&
                         addr_in_rom(reg_const[rm].value))
                    next_record={true,reg_const[rm].value,lhs.scale,lhs.signed_index};
            }
        }
        for (uint8_t reg=0;reg<16;++reg) if (may_clobber_register(ins,reg)) {
            byte_index[reg]={}; record_address[reg]={};
            byte_domain[reg]={};
        }
        if(ins.is_call || ins.op==armv4t::IrOp::BL_prefix || ins.op==armv4t::IrOp::BL_suffix)
            for(auto& domain:byte_domain) domain={};
        if (ins.rd<15 && ins.cond==armv4t::Cond::AL) {
            if (next_byte.known) byte_index[ins.rd]=next_byte;
            if (next_record.known) record_address[ins.rd]=next_record;
            if (next_domain.known) byte_domain[ins.rd]=next_domain;
        }

        // CMP-bound capture for exact jump-table sizing (MC-HP-000). The
        // canonical switch guard is `CMP Ri,#N` immediately followed by a
        // `B{hi,cs}` to the default case; the in-range index span gives the
        // table's entry count with no guessing. Record the compare, then on
        // the very next instruction (the guard branch) stamp reg_bound[Ri].
        // The bound then rides the index through the scale/add chain below.
        if (ins.op == armv4t::IrOp::CMP &&
            ins.op2.kind == armv4t::Op2::Kind::Imm && ins.rn < 16) {
            last_cmp.known = true;
            last_cmp.reg   = ins.rn;
            last_cmp.imm   = ins.op2.imm_value;
            last_cmp.pc    = ins.pc;
        } else if (last_cmp.known && ins.is_branch &&
                   ins.pc == last_cmp.pc + step) {
            // Max in-range index from the compare. HI/LS bound at N
            // (Ri {>,<=} N); CS/CC bound at N-1 (Ri {>=,<} N).
            bool have = false; uint32_t maxi = 0;
            if (ins.cond == armv4t::Cond::HI ||
                ins.cond == armv4t::Cond::LS) {
                have = true; maxi = last_cmp.imm;
            } else if ((ins.cond == armv4t::Cond::CS ||
                        ins.cond == armv4t::Cond::CC) && last_cmp.imm > 0) {
                have = true; maxi = last_cmp.imm - 1u;
            }
            if (have) {
                // HI/CS: taken branches AWAY to the default, so the
                // indexed dispatch is the fall-through of THIS walk —
                // set the bound directly. LS/CC: taken branches TO the
                // dispatch, which the finder walks as its own seed —
                // park the bound for that target (re-seeded at entry).
                if (ins.cond == armv4t::Cond::HI ||
                    ins.cond == armv4t::Cond::CS) {
                    reg_bound[last_cmp.reg] = {true, maxi};
                    relative_bound_epoch[last_cmp.reg] = relative_epoch;
                } else if (ins.branch_target != 0) {
                    branch_target_bounds_[ins.branch_target] =
                        SeedBound{last_cmp.reg, maxi};
                }
            }
            last_cmp.known = false;
        } else {
            // any other instruction ends the (adjacent-only) cmp window
            last_cmp.known = false;
        }

        if (ins.op == armv4t::IrOp::STR) {
            uint8_t rn = 0;
            int32_t offset = 0;
            uint32_t stored = 0;
            bool known_value = false;
            if (ins.rd == 15) {
                stored = ins.pc + (entry_mode == CpuMode::Thumb ? 4u : 12u);
                if (entry_mode == CpuMode::Thumb) {
                    stored &= ~uint32_t{3};
                }
                known_value = true;
            } else if (ins.rd < 16 && reg_const[ins.rd].known) {
                stored = reg_const[ins.rd].value;
                known_value = true;
            }

            if (entry_mode == CpuMode::Thumb && known_value &&
                ins.cond == armv4t::Cond::AL && !ins.mem.writeback)
                harvest_stored_callback(stored);

            // The real GBA BIOS branches through this word with BX on IRQ.
            // A proved word store therefore supplies a typed code reference,
            // including leaf ARM dispatchers without a PUSH-LR prologue.
            // Discover its ROM body so the existing byte-guarded RAM catalog
            // can also serve it when the game later copies it into IWRAM.
            uint32_t vector_slot = 0;
            if (known_value && ins.cond == armv4t::Cond::AL &&
                !ins.mem.writeback && eval_mem_imm_addr(ins, &vector_slot) &&
                vector_slot == 0x03007FFCu) {
                const uint32_t target = stored & ~uint32_t{1};
                const CpuMode mode = (stored & 1u) ? CpuMode::Thumb : CpuMode::Arm;
                if (target >= 0x08000000u && addr_in_rom(target) &&
                    looks_like_code(target, mode, /*strict=*/true) &&
                    aot_seen_seed_keys_.insert(visit_key(target, mode)).second) {
                    aot_pointer_seeds_.push_back({target, mode, "", 0, false});
                    ++stats_.aot_callback_seeds;
                }
            }

            if (!ins.mem.writeback &&
                eval_symbolic_mem_addr(ins, &rn, &offset) &&
                known_value) {
                remember_mem_const(rn, offset, stored);
            } else {
                clear_mem_const();
            }
        } else if (ins.op == armv4t::IrOp::STRB ||
                   ins.op == armv4t::IrOp::STRH) {
            clear_mem_const();
        } else if (ins.op == armv4t::IrOp::STM && !ins.block.load) {
            if (ins.block.rn >= 15 || ins.block.reg_list == 0) {
                clear_mem_const();
            } else if (ins.block.writeback &&
                       reg_sym[ins.block.rn].known) {
                clear_mem_const();
            } else {
                if (ins.block.writeback) {
                    invalidate_mem_base(ins.block.rn);
                }
                for (int reg = 0; reg < 16; ++reg) {
                    if ((ins.block.reg_list & (1u << reg)) == 0) continue;
                    int32_t offset = 0;
                    if (!eval_block_reg_offset(
                            ins, reg,
                            /*relative_to_writeback_base=*/true,
                            &offset)) {
                        clear_mem_const();
                        break;
                    }

                    uint32_t stored = 0;
                    bool known_value = false;
                    if (reg == 15) {
                        stored = ins.pc +
                            (entry_mode == CpuMode::Thumb ? 4u : 12u);
                        if (entry_mode == CpuMode::Thumb) {
                            stored &= ~uint32_t{3};
                        }
                        known_value = true;
                    } else if (reg_const[reg].known) {
                        stored = reg_const[reg].value;
                        known_value = true;
                    }

                    uint8_t rn = 0;
                    int32_t resolved_offset = 0;
                    bool resolved_base = resolve_symbolic_base(
                        ins.block.rn, offset, &rn, &resolved_offset);

                    if (known_value && resolved_base) {
                        remember_mem_const(rn, resolved_offset, stored);
                    } else if (resolved_base) {
                        forget_mem_const(rn, resolved_offset);
                    } else {
                        clear_mem_const();
                    }
                }
            }
        } else if (ins.op == armv4t::IrOp::LDM && ins.block.load) {
            // Every listed register is overwritten from memory. This is
            // especially important for the common THUMB return veneer
            // `pop {rN}; bx rN`: retaining an older literal constant for rN
            // invents a static BX target and can make data decode as code.
            uint16_t loaded = ins.block.reg_list;
            if (loaded == 0) loaded = static_cast<uint16_t>(1u << 15);
            for (uint8_t reg = 0; reg < 16; ++reg) {
                if ((loaded & (1u << reg)) == 0) continue;
                reg_const[reg].known = false;
                reg_sym[reg].known = false;
                reg_scaled[reg].known = false;
                reg_table[reg].known = false;
                reg_bound[reg].known = false;
                invalidate_relative_source(reg);
                invalidate_mem_base(reg);
                invalidate_sym_base(reg);
            }
            if (ins.block.writeback && ins.block.rn < 16) {
                reg_const[ins.block.rn].known = false;
                reg_sym[ins.block.rn].known = false;
                reg_scaled[ins.block.rn].known = false;
                reg_table[ins.block.rn].known = false;
                reg_bound[ins.block.rn].known = false;
                invalidate_relative_source(ins.block.rn);
                invalidate_mem_base(ins.block.rn);
                invalidate_sym_base(ins.block.rn);
            }
        }

        // Snapshot Rd's tracked constant BEFORE the const-tracker below
        // clears it. The jump-table-address former (further down) reads the
        // base operand's constant; for the 2-operand fold `ADD Rd,Rd,Rm`
        // (Rd==Rn, e.g. THUMB `add r0,ip`) the base lives in Rd, which the
        // tracker is about to invalidate. Reading this snapshot for the
        // Rd operand recovers the pre-instruction base. (Rd is the only
        // register the const-tracker mutates this step, so other operands
        // are unaffected.)
        const bool     rd_was_const = (ins.rd < 16) && reg_const[ins.rd].known;
        const uint32_t rd_const_val =
            (ins.rd < 16) ? reg_const[ins.rd].value : 0u;

        // Update the narrow constant tracker. Do this AFTER the
        // BX check above (we want to read the value as-of the BX,
        // not after the BX clears it).
        //
        // MOV Rd, #imm  →  track value
        // ADD Rd, PC, #imm  →  track (pc+8 ARM, pc+4 THUMB) + imm
        // Anything else writing Rd  →  invalidate
        if (ins.rd < 16 && may_clobber_register(ins,ins.rd)) {
            // Thumb compares carry their input in Rd in the decoded IR,
            // but leave the source index intact for a subsequent guard.
            if (!ins.is_branch &&
                ins.op != armv4t::IrOp::CMP && ins.op != armv4t::IrOp::CMN &&
                ins.op != armv4t::IrOp::TST && ins.op != armv4t::IrOp::TEQ)
                invalidate_relative_source(ins.rd);
            if (ins.op == armv4t::IrOp::MOV &&
                ins.op2.kind == armv4t::Op2::Kind::Imm) {
                reg_const[ins.rd].known = true;
                reg_const[ins.rd].value = ins.op2.imm_value;
                reg_sym[ins.rd].known = false;
            } else if (ins.op == armv4t::IrOp::MOV &&
                       ins.op2.kind == armv4t::Op2::Kind::Shifted &&
                       !ins.op2.shifted.by_register &&
                       ins.op2.shifted.imm_or_rs == 0 &&
                       ins.op2.shifted.rm < 16) {
                uint8_t rm = ins.op2.shifted.rm;
                if (rm == 15) {
                    uint32_t pc_val = ins.pc +
                        (entry_mode == CpuMode::Thumb ? 4u : 8u);
                    if (entry_mode == CpuMode::Thumb) pc_val &= ~uint32_t{3};
                    reg_const[ins.rd].known = true;
                    reg_const[ins.rd].value = pc_val;
                } else if (reg_const[rm].known) {
                    reg_const[ins.rd].known = true;
                    reg_const[ins.rd].value = reg_const[rm].value;
                } else {
                    reg_const[ins.rd].known = false;
                }
                if (rm < 15) {
                    if (reg_sym[rm].known) {
                        reg_sym[ins.rd] = reg_sym[rm];
                    } else {
                        reg_sym[ins.rd].known = true;
                        reg_sym[ins.rd].rn = rm;
                        reg_sym[ins.rd].offset = 0;
                    }
                } else {
                    reg_sym[ins.rd].known = false;
                }
            } else if ((ins.op == armv4t::IrOp::ADD ||
                        (ins.op == armv4t::IrOp::SUB && ins.cond == armv4t::Cond::AL)) &&
                       ins.rn == 15 &&
                       ins.op2.kind == armv4t::Op2::Kind::Imm) {
                uint32_t pc_val = ins.pc +
                    (entry_mode == CpuMode::Thumb ? 4u : 8u);
                if (entry_mode == CpuMode::Thumb) pc_val &= ~uint32_t{3};
                reg_const[ins.rd].known = true;
                reg_const[ins.rd].value = ins.op == armv4t::IrOp::SUB
                    ? pc_val - ins.op2.imm_value : pc_val + ins.op2.imm_value;
                reg_sym[ins.rd].known = false;
                harvest_aot_reference(reg_const[ins.rd].value);
            } else if ((ins.op == armv4t::IrOp::ADD ||
                        ins.op == armv4t::IrOp::SUB) &&
                       ins.rn < 16 &&
                       ins.op2.kind == armv4t::Op2::Kind::Imm &&
                       reg_const[ins.rn].known) {
                reg_const[ins.rd].known = true;
                if (ins.op == armv4t::IrOp::ADD) {
                    reg_const[ins.rd].value =
                        reg_const[ins.rn].value + ins.op2.imm_value;
                } else {
                    reg_const[ins.rd].value =
                        reg_const[ins.rn].value - ins.op2.imm_value;
                }
                reg_sym[ins.rd].known = false;
            } else if (ins.op == armv4t::IrOp::LDR &&
                       !ins.mem.writeback) {
                uint32_t ea = 0;
                if (eval_mem_imm_addr(ins, &ea) && can_read_at(ea, 4)) {
                    uint32_t raw = read_u32(ea & ~uint32_t{3});
                    raw = ror32(raw, (ea & 3u) * 8u);
                    reg_const[ins.rd].known = true;
                    reg_const[ins.rd].value = raw;
                    // Harvest only true PC-relative literal pools
                    // (`[pc,#imm]`). reg_const-based pointer-table loads
                    // are intentionally excluded here (higher false-
                    // positive rate); a BX/BLX/MOV-pc consumer of a
                    // tracked pointer is already resolved separately.
                    if (ins.mem.rn == 15 && !ins.mem.by_register) {
                        maybe_harvest_literal(raw);
                        // If the literal is a ROM data address, inspect only
                        // that referenced location as a possible callback
                        // table. This reachability evidence avoids the
                        // catastrophic false-positive rate of sweeping every
                        // pointer-shaped word in asset/compressed-data space.
                        harvest_aot_reference(raw);
                    }
                } else {
                    reg_const[ins.rd].known = false;
                }
                reg_sym[ins.rd].known = false;
            } else {
                // Any other write to Rd invalidates the tracker
                // entry. Unsupported value computations remain unknown.
                reg_const[ins.rd].known = false;
                reg_sym[ins.rd].known = false;
            }
            invalidate_mem_base(ins.rd);
            invalidate_sym_base(ins.rd);
            relative_base_epoch[ins.rd] =
                reg_const[ins.rd].known &&
                ((ins.op == armv4t::IrOp::ADD && ins.rn == 15 &&
                  ins.op2.kind == armv4t::Op2::Kind::Imm) ||
                 (ins.op == armv4t::IrOp::LDR && ins.mem.rn == 15 &&
                  !ins.mem.by_register)) ? relative_epoch : 0;
        }

        // Maintain the jump-table address trackers (independent of the
        // reg_const tracker above; consumed by the LDR detection). A
        // write to Rd clears its scaled/table state unless this same
        // instruction re-establishes it.
        if (ins.rd < 16) {
            bool set_scaled = false, set_table = false;
            const armv4t::Op2& o = ins.op2;
            // Base operand's tracked constant, reading the pre-instruction
            // snapshot for Rd (rd_was_const/rd_const_val) so a 2-operand
            // fold `ADD Rd,Rd,Rm` can still recover the base Rd held before
            // the const-tracker cleared it.
            auto base_known = [&](uint8_t r) {
                return (r == ins.rd) ? rd_was_const : reg_const[r].known;
            };
            auto base_val = [&](uint8_t r) {
                return (r == ins.rd) ? rd_const_val : reg_const[r].value;
            };
            auto backed_const = [&](uint8_t r) {
                return base_known(r) && can_read_at(base_val(r),4);
            };
            // Resolve a scaled index's range bound, falling back to the
            // source index register's CMP bound when the switch guard came
            // AFTER the scale (so bound_known is still false on the scale).
            auto scaled_bound = [&](const RegScaled& s,
                                    bool* known, uint32_t* maxi) {
                if (s.bound_known) { *known = true; *maxi = s.max_index; }
                else if (s.src_reg < 16 && reg_bound[s.src_reg].known) {
                    *known = true; *maxi = reg_bound[s.src_reg].max_index;
                } else { *known = false; }
            };
            if (ins.op == armv4t::IrOp::MOV &&
                o.kind == armv4t::Op2::Kind::Shifted &&
                !o.shifted.by_register &&
                o.shifted.type == armv4t::ShiftType::LSL &&
                o.shifted.imm_or_rs > 0 && o.shifted.rm < 16) {
                // Rd = (unknown index) << k
                reg_scaled[ins.rd].known = true;
                reg_scaled[ins.rd].shift = o.shifted.imm_or_rs;
                reg_scaled[ins.rd].bound_known = false;
                reg_scaled[ins.rd].src_reg = o.shifted.rm;
                reg_scaled[ins.rd].relative_bound_epoch =
                    relative_bound_epoch[o.shifted.rm];
                // A later guard constrains this scale only while its source
                // still holds the original index. In-place scaling requires
                // a bound captured before the scale.
                reg_scaled[ins.rd].relative_source_live = ins.rd != o.shifted.rm;
                if (reg_bound[o.shifted.rm].known) {
                    reg_scaled[ins.rd].bound_known = true;
                    reg_scaled[ins.rd].max_index =
                        reg_bound[o.shifted.rm].max_index;
                }
                set_scaled = true;
            } else if (ins.op == armv4t::IrOp::MOV &&
                       o.kind == armv4t::Op2::Kind::Shifted &&
                       !o.shifted.by_register &&
                       o.shifted.type == armv4t::ShiftType::LSL &&
                       o.shifted.imm_or_rs == 0 && o.shifted.rm < 16 &&
                       o.shifted.rm != ins.rd) {
                // Plain register move `MOV Rd, Rm` (e.g. THUMB `mov ip,r0`)
                // staging a scaled switch index through a scratch register.
                // Propagate the scaled/table state so a later `ADD base,Rd`
                // still forms the table. (rm != rd guards a self-move.)
                const uint8_t rm = o.shifted.rm;
                if (reg_scaled[rm].known) {
                    reg_scaled[ins.rd] = reg_scaled[rm];
                    set_scaled = true;
                }
                if (reg_table[rm].known) {
                    reg_table[ins.rd] = reg_table[rm];
                    set_table = true;
                }
            } else if (ins.op == armv4t::IrOp::ADD &&
                       o.kind == armv4t::Op2::Kind::Shifted &&
                       !o.shifted.by_register && o.shifted.rm < 16 &&
                       ins.rn < 16) {
                const uint8_t rm = o.shifted.rm;
                if (o.shifted.type == armv4t::ShiftType::LSL &&
                    o.shifted.imm_or_rs > 0) {
                    // One-instruction scaled add: ADD Rd, Rbase, Ri LSL#k
                    // (Ri is the inline-scaled index; bound rides on it.)
                    if (backed_const(ins.rn)) {
                        reg_table[ins.rd] = {true, base_val(ins.rn),
                                             1u << o.shifted.imm_or_rs};
                        if (reg_bound[rm].known) {
                            reg_table[ins.rd].bound_known = true;
                            reg_table[ins.rd].max_index =
                                reg_bound[rm].max_index;
                        }
                        set_table = true;
                    }
                } else if (o.shifted.imm_or_rs == 0) {
                    // Plain reg add: one side a ROM-pointer base, the other a
                    // previously-scaled index (bound carried on the scale, or
                    // recovered from the index's source CMP bound).
                    bool bk = false; uint32_t mi = 0;
                    if (backed_const(ins.rn) && reg_scaled[rm].known) {
                        reg_table[ins.rd] = {true, base_val(ins.rn),
                                             1u << reg_scaled[rm].shift};
                        scaled_bound(reg_scaled[rm], &bk, &mi);
                        if (bk) {
                            reg_table[ins.rd].bound_known = true;
                            reg_table[ins.rd].max_index = mi;
                        }
                        set_table = true;
                    } else if (backed_const(rm) && reg_scaled[ins.rn].known) {
                        reg_table[ins.rd] = {true, base_val(rm),
                                             1u << reg_scaled[ins.rn].shift};
                        scaled_bound(reg_scaled[ins.rn], &bk, &mi);
                        if (bk) {
                            reg_table[ins.rd].bound_known = true;
                            reg_table[ins.rd].max_index = mi;
                        }
                        set_table = true;
                    } else if (backed_const(ins.rn) &&
                               !(rm == ins.rd ? rd_was_const : reg_const[rm].known)) {
                        // The byte offset may arrive already scaled from an
                        // earlier CFG block (e.g. after an MSR boundary). Keep
                        // the ROM base without claiming an index bound. A word
                        // load and a later branch consumer must still prove
                        // this is a callback table before entries are emitted.
                        reg_table[ins.rd] = {true, base_val(ins.rn), 0};
                        set_table = true;
                    } else if (backed_const(rm) &&
                               !(ins.rn == ins.rd ? rd_was_const : reg_const[ins.rn].known)) {
                        reg_table[ins.rd] = {true, base_val(rm), 0};
                        set_table = true;
                    }
                }
            }
            if (!set_scaled) reg_scaled[ins.rd].known = false;
            if (!set_table)  reg_table[ins.rd].known = false;
            // The cmp-derived bound has been consumed into reg_scaled/
            // reg_table above; any non-branch write to Rd retires it. A
            // branch is skipped because the guard branch itself is what
            // SET the bound (see the CMP-bound capture above).
            if (!ins.is_branch) reg_bound[ins.rd].known = false;
        }

        if ((ins.op == armv4t::IrOp::LDR ||
             ins.op == armv4t::IrOp::LDRB ||
             ins.op == armv4t::IrOp::LDRH ||
             ins.op == armv4t::IrOp::LDRSB ||
             ins.op == armv4t::IrOp::LDRSH ||
             ins.op == armv4t::IrOp::STR ||
             ins.op == armv4t::IrOp::STRB ||
             ins.op == armv4t::IrOp::STRH) &&
            ins.mem.writeback && ins.mem.rn < 16) {
            reg_const[ins.mem.rn].known = false;
            reg_sym[ins.mem.rn].known = false;
            reg_scaled[ins.mem.rn].known = false;
            reg_table[ins.mem.rn].known = false;
            relative_bound_epoch[ins.mem.rn] = 0;
            invalidate_relative_source(ins.mem.rn);
            invalidate_mem_base(ins.mem.rn);
            invalidate_sym_base(ins.mem.rn);
        }

        pc += step;
        if(ins.is_branch || ins.is_call || ins.op==armv4t::IrOp::SWI) {
            callback_branch_start=pc;
            callback_branch_constant=false;
        }
        if (ins.is_call || ins.op == armv4t::IrOp::SWI) {
            ++relative_epoch;
            relative.active = false;
            for (auto& scaled : reg_scaled) scaled.relative_source_live = false;
        }
        if (!in_switch_block) fn.end_addr = pc;

        // A conditional indirect transfer is not a terminator when
        // its condition fails. Because the finder stops at indirects,
        // seed the fall-through root explicitly.
        if ((ins.is_return || ins.is_indirect) &&
            ins.cond != armv4t::Cond::AL) {
            enqueue_resolved_target(pc, entry_mode, "branch");
        }

        // Stop at unconditional terminators:
        //   1. A return-shaped instruction (BX LR, pop {pc}, ldm
        //      r?, {..., pc}).
        //   2. An indirect control transfer.
        //   3. An unconditional branch (B with cond=AL) — we'll
        //      reach the target by adding it to the worklist.
        if (ins.is_return || ins.is_indirect) {
            if (next_switch_block()) continue;
            break;
        }
        const bool unconditional_branch =
            ins.is_branch &&
            ins.cond == armv4t::Cond::AL &&
            !ins.is_call;
        if (unconditional_branch) {
            if (next_switch_block()) continue;
            break;
        }
    }

    // Preserve the final case's exact extent (including a budget-limited walk).
    if (in_switch_block && pc > block_start &&
        (fn.switch_blocks.empty() || fn.switch_blocks.back().start != block_start))
        fn.switch_blocks.push_back({block_start, pc});

    if (current_walk_dropped_) {
        // A speculative literal-pool function entered a data_range — not
        // real code. Drop it silently (no hard error). Erase the visited
        // marker so a genuine, authoritative path can rediscover the
        // address later (and that path WILL hard-error if it too enters
        // a data_range — surfacing a real contradiction). Discard any
        // sub-seeds harvested during this discarded walk.
        visited_.erase(key);
        mode_switch_seeds_.clear();
        literal_pool_seeds_.clear();
        aot_pointer_seeds_.clear();
        current_walk_dropped_ = false;
        return;  // do NOT push_back fn
    }

    // Preserve the true (un-clamped) body-walk extent before run()'s
    // "next function start" clamp truncates end_addr (see Function::walk_end_addr).
    fn.walk_end_addr = fn.end_addr;
    deferred_callback_branches_.insert(function_callback_branches.begin(), function_callback_branches.end());
    functions_.push_back(std::move(fn));
}

void FunctionFinder::run(std::size_t max_functions) {
    const std::size_t manual_seed_count = seeds_.size();
    std::unordered_set<uint64_t> manual_seed_keys;
    for (const auto& s : seeds_)
        manual_seed_keys.insert(visit_key(s.addr, s.mode));
    std::vector<FunctionSeed> code_copy_seeds = collect_code_copy_seeds();
    stats_.aot_code_copy_seeds = code_copy_seeds.size();
    seeds_.insert(seeds_.end(),
                  code_copy_seeds.begin(), code_copy_seeds.end());
    auto callback_seeds = collect_referenced_callback_seeds();
    seeds_.insert(seeds_.end(), callback_seeds.begin(), callback_seeds.end());

    // Validate seeds against data_ranges before any walking. A seed
    // landing inside a data_range is a collision (the TOML author
    // is contradicting themselves; or a jump_table expanded onto
    // bytes that another data_range marked as "not code").
    for (const auto& s : seeds_) {
        if (addr_in_data_range(s.addr)) {
            record_collision(s.addr, 0, s.name, "seed");
        }
    }

    // Snapshot seed (addr, mode) keys so we can later compute the
    // manual-seed/walk-overlap breakdown for the discovery summary.
    std::unordered_set<uint64_t> seed_keys;
    std::unordered_map<uint64_t, FunctionSeed> seed_by_key;
    for (const auto& s : seeds_) {
        uint64_t k = visit_key(s.addr, s.mode);
        seed_keys.insert(k);
        seed_by_key.emplace(k, s);
    }
    stats_.manual_seeds_total = manual_seed_count;

    // Dedup-at-push: every (addr,mode) ever queued. discover_one already
    // dedups at visit time, so the worklist otherwise fills with
    // already-queued/visited no-ops — on Minish Cap it bloated to ~170k
    // of mostly-duplicate branch targets. Skipping a re-push is
    // output-neutral (the discovered closure is unchanged) and is the
    // dominant regen-speed cost. Seeded with the initial seed keys.
    std::unordered_set<uint64_t> enqueued = seed_keys;

    struct QueuedSeed {
        FunctionSeed seed;
        bool required;
    };

    // Seed worklist. Walk-discovered control-flow roots are inserted
    // immediately after the current item so local continuations cannot
    // be starved behind thousands of metadata seeds when max_functions
    // is intentionally bounded during bring-up.
    std::vector<QueuedSeed> worklist;
    worklist.reserve(seeds_.size());
    for (std::size_t i = 0; i < seeds_.size(); ++i) {
        worklist.push_back(QueuedSeed{
            seeds_[i], i < manual_seed_count});
    }
    std::size_t required_remaining = manual_seed_count;

    // Per-key origin marker: was this (addr, mode) reached as a
    // seed, by walk, or both? Walks add "walk"; seeds add "seed".
    // Both → "redundant_manual".
    std::unordered_map<uint64_t, uint8_t> origin_mask;  // 1=seed, 2=walk
    for (std::size_t i = 0; i < seeds_.size(); ++i) {
        origin_mask[visit_key(seeds_[i].addr, seeds_[i].mode)] |=
            (i < manual_seed_count) ? 1u : 2u;
    }

    std::size_t pos = 0;
    while ((pos < worklist.size() || !deferred_store_callbacks_.empty() || !deferred_callback_branches_.empty()) &&
           (functions_.size() < max_functions || required_remaining > 0)) {
        if(pos==worklist.size()) {
            if (deferred_store_callbacks_.empty()) {
                // The branch extension must not spend the bounded call-site
                // budget, or change traversal provenance, before the existing
                // discovery closure completes. Recursive branch calls use a
                // separate budget too; only completed code walks add roots.
                const auto branches=std::move(deferred_callback_branches_);
                deferred_callback_branches_.clear();
                for (const auto pc:branches) harvest_contextual_callbacks(pc,0,{},0,true);
                for (const auto& seed:aot_pointer_seeds_) {
                    const auto k=visit_key(seed.addr,seed.mode);
                    origin_mask[k]|=2u;
                    if (enqueued.insert(k).second) worklist.push_back({seed,false});
                }
                aot_pointer_seeds_.clear();
                if (pos==worklist.size()) continue;
            }
            // Establish the existing reachable closure before adding these
            // conservative metadata roots. Preseeding an already reachable
            // callback would otherwise change its walk's required/speculative
            // provenance and could suppress discovery from that real caller.
            for(const auto& seed:deferred_store_callbacks_) {
                const uint64_t key=visit_key(seed.addr,seed.mode);
                if(!enqueued.insert(key).second) continue;
                aot_seen_seed_keys_.insert(key);
                origin_mask[key]|=2u;
                worklist.push_back({seed,false});
                ++stats_.aot_callback_seeds;
            }
            deferred_store_callbacks_.clear();
            continue;
        }
        // Safety net: discovery must converge (worklist drains toward
        // empty). A runaway past this bound means a detector or decoder
        // change is seeding garbage — halt loudly rather than exhaust
        // memory. Baseline Minish Cap discovery peaks near ~170k, so
        // 2M is far above any healthy run.
        if (worklist.size() > 2000000u) {
            std::fprintf(stderr,
                "[finder] WORKLIST BRAKE at %zu (pos=%zu functions=%zu) "
                "— halting; discovery is not converging\n",
                worklist.size(), pos, functions_.size());
            std::fflush(stderr);
            break;
        }
        QueuedSeed queued = worklist[pos++];
        if (queued.required && required_remaining > 0) {
            --required_remaining;
        }

        FunctionSeed s = queued.seed;
        uint64_t s_key = visit_key(s.addr, s.mode);
        auto seed_it = seed_by_key.find(s_key);
        bool current_required =
            queued.required || manual_seed_keys.count(s_key) != 0;
        if (!current_required && functions_.size() >= max_functions) {
            continue;
        }
        if (seed_it != seed_by_key.end()) {
            if (!seed_it->second.name.empty()) {
                s.name = seed_it->second.name;
            }
            if (s.source_addr == 0) {
                s.source_addr = seed_it->second.source_addr;
            }
        }

        std::size_t before = functions_.size();
        discover_one(s.addr, s.mode, s.name, s.source_addr, s.speculative);
        if (functions_.size() == before) continue;

        // The function we just added is at the end of functions_.
        const Function& fn = functions_.back();
        std::vector<QueuedSeed> discovered_seeds;

        // Each direct branch target becomes a new seed at the SAME
        // mode (control flow within a function doesn't switch
        // ARM/THUMB without a BX, which is indirect).
        for (uint32_t t : fn.direct_branch_targets) {
            uint64_t k = visit_key(t, fn.mode);
            // Mark walk-origin. If this address was ALSO a seed,
            // it now has both bits set (redundant_manual).
            origin_mask[k] |= 2u;
            if (!enqueued.insert(k).second) continue;  // already queued
            // Propagate speculation: a branch target reached only from a
            // speculative (literal-pool) walk is itself speculative.
            uint32_t target_source = 0;
            const CodeCopyRange* caller_copy=nullptr;
            const CodeCopyRange* target_copy=nullptr;
            for (const auto& copy : code_copies_) {
                if (!caller_copy && fn.addr>=copy.runtime_start &&
                    fn.addr-copy.runtime_start<copy.size)
                    caller_copy=&copy;
                if (!target_copy && t>=copy.runtime_start && t-copy.runtime_start<copy.size)
                    target_copy=&copy;
            }
            if (target_copy && target_copy!=caller_copy) {
                // A call between separately relocated regions uses the
                // callee's declared mapping, not the caller's source bias.
                target_source=target_copy->source_start+(t-target_copy->runtime_start);
            } else if (fn.source_addr != fn.addr) {
                const int64_t delta = static_cast<int64_t>(t) -
                    static_cast<int64_t>(fn.addr);
                const int64_t mapped = static_cast<int64_t>(fn.source_addr) +
                    delta;
                if (mapped >= 0 && mapped <= 0xFFFFFFFFll &&
                    addr_in_rom(static_cast<uint32_t>(mapped))) {
                    target_source = static_cast<uint32_t>(mapped);
                }
            }
            discovered_seeds.push_back(
                QueuedSeed{FunctionSeed{t, fn.mode, "", target_source,
                                        s.speculative},
                           current_required});
            if (current_required) {
                ++required_remaining;
            }
        }
        // Drain any mode-switching seeds discovered during this
        // function's walk (BX Rm to a tracked-constant THUMB
        // target from ARM code, and vice versa).
        for (const auto& ms : mode_switch_seeds_) {
            uint64_t k = visit_key(ms.addr, ms.mode);
            origin_mask[k] |= 2u;
            if (!enqueued.insert(k).second) continue;  // already queued
            discovered_seeds.push_back(QueuedSeed{ms, current_required});
            if (current_required) {
                ++required_remaining;
            }
        }
        mode_switch_seeds_.clear();
        // Drain speculative literal-pool seeds harvested during this
        // walk (PC-relative loads of words that look like code pointers).
        // Same enqueue path as mode_switch_seeds_; the worklist dedups
        // against anything already discovered, so a literal that points
        // at a known function is harmlessly dropped here.
        for (const auto& ls : literal_pool_seeds_) {
            uint64_t k = visit_key(ls.addr, ls.mode);
            origin_mask[k] |= 2u;
            if (!enqueued.insert(k).second) continue;  // already queued
            discovered_seeds.push_back(QueuedSeed{ls, current_required});
            if (current_required) {
                ++required_remaining;
            }
        }
        literal_pool_seeds_.clear();
        harvest_relative_callbacks();
        // Drain address-taken functions from pointer tables whose base was
        // itself loaded by this reachable walk.
        for (const auto& as : aot_pointer_seeds_) {
            uint64_t k = visit_key(as.addr, as.mode);
            origin_mask[k] |= 2u;
            if (!enqueued.insert(k).second) continue;
            discovered_seeds.push_back(QueuedSeed{as, current_required});
            if (current_required) {
                ++required_remaining;
            }
        }
        aot_pointer_seeds_.clear();
        if (!discovered_seeds.empty()) {
            worklist.insert(worklist.begin() + static_cast<std::ptrdiff_t>(pos),
                            discovered_seeds.begin(),
                            discovered_seeds.end());
        }
        // If THIS function came from a seed (not a walk), it was
        // discovered "as a seed", but the function's body still
        // counts toward "walked from here" for its branches.
        uint64_t this_key = visit_key(fn.addr, fn.mode);
        if (seed_keys.count(this_key) == 0) {
            // It came from a walk (a branch target reached during
            // a prior function's discovery).
            origin_mask[this_key] |= 2u;
        }
    }

    // Apply exclude_func filter — remove functions whose entry
    // address was declared in [[exclude_func]].
    if (!excluded_.empty()) {
        std::size_t before = functions_.size();
        auto last = std::remove_if(functions_.begin(), functions_.end(),
            [&](const Function& fn) {
                return excluded_.count(fn.addr) > 0;
            });
        functions_.erase(last, functions_.end());
        stats_.excluded_count = before - functions_.size();
    }

    // Sort by address.
    std::sort(functions_.begin(), functions_.end(),
              [](const Function& a, const Function& b) {
                  if (a.addr != b.addr) return a.addr < b.addr;
                  return a.mode < b.mode;
              });

    auto is_alias_candidate = [&](const Function& f) -> bool {
        if (alias_candidate_keys_.empty()) return false;
        uint64_t k = (static_cast<uint64_t>(f.addr) << 1) |
                     (f.mode == CpuMode::Thumb ? 1u : 0u);
        return alias_candidate_keys_.count(k) != 0;
    };

    auto source_bias = [](const Function& f) -> int64_t {
        return static_cast<int64_t>(f.source_addr) -
            static_cast<int64_t>(f.addr);
    };

    // Set end_addr to the next same-mode function start when adjacent
    // (approximate boundary). ARM and THUMB entries occupy the same byte
    // address space but do not share instruction boundaries; a false-start
    // THUMB entry in the middle of ARM code must not truncate the ARM body.
    // Explicit interior/resume seeds (alias candidates) are NOT boundaries —
    // skipping them keeps the host's body intact so the candidate can be
    // rolled into it as a mid-function label rather than splitting the host.
    for (std::size_t i = 0; i < functions_.size(); ++i) {
        if (is_alias_candidate(functions_[i])) continue;  // extent unused
        for (std::size_t j = i + 1; j < functions_.size(); ++j) {
            if (functions_[j].mode != functions_[i].mode) continue;
            // Overlapping RAM placements of the same immutable routine are
            // separate static CFGs. A function from another placement must
            // not truncate this one's emitted body merely because its runtime
            // address happens to fall in the interval.
            if (source_bias(functions_[j]) != source_bias(functions_[i])) {
                continue;
            }
            if (is_alias_candidate(functions_[j])) continue;  // not a boundary
            if (functions_[j].addr > functions_[i].addr) {
                functions_[i].end_addr = std::min(
                    functions_[i].end_addr,
                    functions_[j].addr);
            }
            break;
        }
    }

    // ── Mid-function aliasing (ported from psxrecomp) ───────────────────────
    // Roll each explicit interior/resume seed into the REAL host whose clamped
    // body contains it (host.addr < cand.addr < host.end_addr, same mode).
    // A WaitForVBlank-style busy-wait that an IRQ returns into mid-loop is the
    // motivating case: rooting the resume PC as its own function would split
    // the tight loop into dispatch-chained 1-instruction functions (binary
    // search per iteration → throughput regression, see psxrecomp). Instead
    // the host stays one whole function (native goto back-edges intact) and the
    // resume PC becomes an interior label reached via the host's resume
    // prologue. Host extents are the just-computed CLAMPED end_addr (accurate
    // next-real-function boundary), so the resume PC is guaranteed to lie in
    // the host's emitted body — its label exists. A candidate with no
    // containing host (a gap / genuine standalone) is left as its own function.
    if (!alias_candidate_keys_.empty()) {
        std::size_t aliases = 0;
        for (std::size_t i = 0; i < functions_.size(); ++i) {
            Function& c = functions_[i];
            if (!is_alias_candidate(c)) continue;
            std::size_t best = SIZE_MAX;
            for (std::size_t j = 0; j < functions_.size(); ++j) {
                if (j == i) continue;
                const Function& h = functions_[j];
                if (is_alias_candidate(h) || h.mode != c.mode) continue;
                if (source_bias(h) != source_bias(c)) continue;
                if (h.addr < c.addr && c.addr < h.end_addr) {
                    if (best == SIZE_MAX ||
                        h.addr > functions_[best].addr) {
                        best = j;
                    }
                }
            }
            if (best != SIZE_MAX) {
                functions_[best].alias_entries.push_back(c.addr);
                c.is_alias = true;
                ++aliases;
            }
        }
        std::size_t hosts = 0;
        for (Function& f : functions_) {
            if (!f.alias_entries.empty()) {
                std::sort(f.alias_entries.begin(), f.alias_entries.end());
                ++hosts;
            }
        }
        stats_.alias_entries_total = aliases;
        stats_.alias_hosts_total = hosts;
        functions_.erase(
            std::remove_if(functions_.begin(), functions_.end(),
                           [](const Function& f) { return f.is_alias; }),
            functions_.end());
    }

    // Full static-resume coverage. IRQ/SWI return may land at any instruction
    // boundary in a native body. Add every interior boundary as an alias after
    // real function boundaries and explicit aliases have been consolidated.
    // The host body stays intact, preserving native straight-line execution
    // and goto back-edges.
    if (static_resume_all_) {
        for (Function& f : functions_) {
            const uint32_t step =
                f.mode == CpuMode::Thumb ? 2u : 4u;
            for (uint32_t pc = f.addr + step;
                 pc < f.end_addr && pc > f.addr;
                 pc += step) {
                f.alias_entries.push_back(pc);
            }
            for (const auto& block : f.switch_blocks) {
                for (uint32_t pc = block.start; pc < block.end; pc += step)
                    f.alias_entries.push_back(pc);
            }
            if (!f.alias_entries.empty()) {
                std::sort(f.alias_entries.begin(), f.alias_entries.end());
                f.alias_entries.erase(
                    std::unique(f.alias_entries.begin(),
                                f.alias_entries.end()),
                    f.alias_entries.end());
            }
        }
        stats_.alias_entries_total = 0;
        stats_.alias_hosts_total = 0;
        for (const Function& f : functions_) {
            stats_.alias_entries_total += f.alias_entries.size();
            if (!f.alias_entries.empty()) ++stats_.alias_hosts_total;
        }
    }

    // Stats
    // Only attach resume labels whose instructions are in emitted spans.
    stats_.alias_entries_total = 0;
    stats_.alias_hosts_total = 0;
    for (Function& fn : functions_) {
        std::erase_if(fn.alias_entries, [&](uint32_t target) {
            if (target >= fn.addr && target < fn.end_addr) return false;
            for (const auto& block : fn.switch_blocks)
                if (target >= block.start && target < block.end) return false;
            return true;
        });
        if (!static_resume_all_) {
            std::sort(fn.alias_entries.begin(), fn.alias_entries.end());
            fn.alias_entries.erase(std::unique(fn.alias_entries.begin(),
                                              fn.alias_entries.end()),
                                   fn.alias_entries.end());
        }
        stats_.alias_entries_total += fn.alias_entries.size();
        if (!fn.alias_entries.empty()) ++stats_.alias_hosts_total;
    }
    stats_.functions_total = functions_.size();
    stats_.functions_arm = 0;
    stats_.functions_thumb = 0;
    stats_.discovered_by_walk_only = 0;
    stats_.redundant_manual = 0;
    stats_.manual_seeds_only = 0;
    for (const auto& fn : functions_) {
        if (fn.mode == CpuMode::Thumb) ++stats_.functions_thumb;
        else                            ++stats_.functions_arm;
    }
    // Walk-source breakdown for emitted functions only.
    // origin_mask: 1=seed, 2=walk, 3=both.
    for (const auto& fn : functions_) {
        uint8_t mask = origin_mask[visit_key(fn.addr, fn.mode)];
        switch (mask) {
            case 1:
                ++stats_.manual_seeds_only;
                break;
            case 2:
                ++stats_.discovered_by_walk_only;
                break;
            case 3:
                ++stats_.redundant_manual;
                break;
            default: break;
        }
    }
}

}  // namespace gbarecomp
