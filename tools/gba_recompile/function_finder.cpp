// function_finder.cpp — implementation.
//
// Discovery is a worklist BFS:
//   - Pop (addr, mode) from the worklist.
//   - If already visited, skip.
//   - Decode instructions from addr forward, in `mode`.
//   - For each direct branch (B / BL / conditional B), record the
//     target and add it to the worklist as a NEW function root (the
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
#include <cstdio>

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

}  // namespace

FunctionFinder::FunctionFinder(const uint8_t* rom_bytes,
                                std::size_t rom_size,
                                uint32_t rom_base)
    : rom_(rom_bytes), rom_size_(rom_size), rom_base_(rom_base) {}

void FunctionFinder::add_seed(const FunctionSeed& seed) {
    seeds_.push_back(seed);
}

void FunctionFinder::add_data_range(uint32_t start, uint32_t end,
                                     const std::string& note) {
    data_ranges_.push_back(DataRange{start, end, note});
}

void FunctionFinder::add_exclude(uint32_t addr,
                                  const std::string& reason) {
    excluded_.insert(addr);
    exclude_reasons_[addr] = reason;
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
    std::size_t off = addr - rom_base_;
    return  static_cast<uint32_t>(rom_[off])
         | (static_cast<uint32_t>(rom_[off + 1]) << 8)
         | (static_cast<uint32_t>(rom_[off + 2]) << 16)
         | (static_cast<uint32_t>(rom_[off + 3]) << 24);
}

uint32_t FunctionFinder::read_u32_public(uint32_t addr) const {
    if (!addr_in_rom(addr)) return 0;
    if ((addr - rom_base_ + 4) > rom_size_) return 0;
    return read_u32(addr);
}

uint16_t FunctionFinder::read_u16(uint32_t addr) const {
    std::size_t off = addr - rom_base_;
    return  static_cast<uint16_t>(rom_[off])
         | (static_cast<uint16_t>(rom_[off + 1]) << 8);
}

void FunctionFinder::discover_one(uint32_t entry_addr, CpuMode entry_mode,
                                   const std::string& seed_name) {
    if (!addr_in_rom(entry_addr)) {
        return;
    }
    uint64_t key = visit_key(entry_addr, entry_mode);
    if (visited_.find(key) != visited_.end()) return;

    Function fn{};
    fn.addr = entry_addr;
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

    while (count++ < kMaxInstrs && addr_in_rom(pc) &&
           (pc - rom_base_ + step) <= rom_size_) {
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
            ins = armv4t::ThumbDecoder::decode(read_u16(pc), pc);
        } else {
            ins = armv4t::ArmDecoder::decode(read_u32(pc), pc);
        }

        if (ins.is_undefined) {
            ++stats_.undefined_instr_count;
            break;
        }

        // Record direct branch targets. The decoder fills
        // branch_target with the absolute guest address.
        if (ins.is_branch && !ins.is_indirect &&
            ins.branch_target != 0) {
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
            addr_in_rom(pc + 2) &&
            (pc - rom_base_ + 4) <= rom_size_) {
            armv4t::Instr lower = armv4t::ThumbDecoder::decode(
                read_u16(pc + 2), pc + 2);
            if (lower.op == armv4t::IrOp::BL_suffix) {
                uint32_t full = ins.branch_target + lower.swi_imm;
                full &= ~uint32_t{1};  // strip THUMB bit if encoded
                if (full != 0 && addr_in_rom(full)) {
                    if (addr_in_data_range(full)) {
                        record_collision(full, fn.addr, fn.name,
                                          "branch");
                    } else {
                        fn.direct_branch_targets.push_back(full);
                        ++stats_.branch_targets_discovered;
                    }
                }
            }
        }

        // Indirect = computed; we can't follow statically — UNLESS
        // our narrow constant tracker resolved the source register
        // to a known value just above the BX (or MOV PC, Rn).
        if (ins.is_indirect) {
            fn.has_indirect_transfer = true;
            ++stats_.indirect_transfer_count;

            // Two cases land here with a tracked register source:
            //   1. BX Rm                  — `ins.rm` is the source
            //   2. MOV PC, Rn (DP-MOV)    — `ins.op2.shifted.rm`
            //      is the source (op2 with no shift)
            // For both, mode is decoded from bit 0 (interworking).
            uint8_t src_reg = 255;  // sentinel
            if (ins.op == armv4t::IrOp::BX && ins.rm < 16) {
                src_reg = ins.rm;
            } else if (ins.rd == 15 &&
                       ins.op2.kind == armv4t::Op2::Kind::Shifted &&
                       !ins.op2.shifted.by_register &&
                       ins.op2.shifted.imm_or_rs == 0 &&
                       ins.op2.shifted.rm < 16) {
                // DP write to PC with op2 = plain register: this is
                // either `mov pc, Rn` (computed jump) or some other
                // DP-PC pattern. If the source register is tracked,
                // we know the target.
                src_reg = ins.op2.shifted.rm;
            }
            if (src_reg < 16 && reg_const[src_reg].known) {
                uint32_t raw = reg_const[src_reg].value;
                uint32_t tgt = raw & ~uint32_t{1};
                CpuMode tgt_mode = (raw & 1u)
                    ? CpuMode::Thumb : CpuMode::Arm;
                if (tgt != 0 && addr_in_rom(tgt) &&
                    !addr_in_data_range(tgt)) {
                    mode_switch_seeds_.push_back(
                        FunctionSeed{tgt, tgt_mode, ""});
                    ++stats_.branch_targets_discovered;
                }
            }
        }

        // Update the narrow constant tracker. Do this AFTER the
        // BX check above (we want to read the value as-of the BX,
        // not after the BX clears it).
        //
        // MOV Rd, #imm  →  track value
        // ADD Rd, PC, #imm  →  track (pc+8 ARM, pc+4 THUMB) + imm
        // Anything else writing Rd  →  invalidate
        if (ins.rd < 16) {
            if (ins.op == armv4t::IrOp::MOV &&
                ins.op2.kind == armv4t::Op2::Kind::Imm) {
                reg_const[ins.rd].known = true;
                reg_const[ins.rd].value = ins.op2.imm_value;
            } else if (ins.op == armv4t::IrOp::ADD &&
                       ins.rn == 15 &&
                       ins.op2.kind == armv4t::Op2::Kind::Imm) {
                uint32_t pc_val = ins.pc +
                    (entry_mode == CpuMode::Thumb ? 4u : 8u);
                reg_const[ins.rd].known = true;
                reg_const[ins.rd].value = pc_val + ins.op2.imm_value;
            } else {
                // Any other write to Rd invalidates the tracker
                // entry. We don't try to detect "no write" — a
                // few false-invalidations (over-clearing) are fine
                // and bias toward conservative behavior.
                reg_const[ins.rd].known = false;
            }
        }

        pc += step;
        fn.end_addr = pc;

        // Stop at unconditional terminators:
        //   1. A return-shaped instruction (BX LR, pop {pc}, ldm
        //      r?, {..., pc}).
        //   2. An indirect control transfer.
        //   3. An unconditional branch (B with cond=AL) — we'll
        //      reach the target by adding it to the worklist.
        if (ins.is_return || ins.is_indirect) {
            break;
        }
        const bool unconditional_branch =
            ins.is_branch &&
            ins.cond == armv4t::Cond::AL &&
            !ins.is_call;
        if (unconditional_branch) {
            break;
        }
    }

    functions_.push_back(std::move(fn));
}

void FunctionFinder::run(std::size_t max_functions) {
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
    for (const auto& s : seeds_) {
        seed_keys.insert(visit_key(s.addr, s.mode));
    }
    stats_.manual_seeds_total = seeds_.size();

    // Seed worklist. BFS by index so we process roots first
    // (entry_point + tmc seeds in TSV order), then branch targets
    // discovered along the way.
    std::vector<FunctionSeed> worklist = seeds_;

    // Per-key origin marker: was this (addr, mode) reached as a
    // seed, by walk, or both? Walks add "walk"; seeds add "seed".
    // Both → "redundant_manual".
    std::unordered_map<uint64_t, uint8_t> origin_mask;  // 1=seed, 2=walk
    for (const auto& s : seeds_) {
        origin_mask[visit_key(s.addr, s.mode)] |= 1u;
    }

    std::size_t pos = 0;
    while (pos < worklist.size() && functions_.size() < max_functions) {
        FunctionSeed s = worklist[pos++];

        std::size_t before = functions_.size();
        discover_one(s.addr, s.mode, s.name);
        if (functions_.size() == before) continue;

        // The function we just added is at the end of functions_.
        const Function& fn = functions_.back();
        // Each direct branch target becomes a new seed at the SAME
        // mode (control flow within a function doesn't switch
        // ARM/THUMB without a BX, which is indirect).
        for (uint32_t t : fn.direct_branch_targets) {
            uint64_t k = visit_key(t, fn.mode);
            // Mark walk-origin. If this address was ALSO a seed,
            // it now has both bits set (redundant_manual).
            origin_mask[k] |= 2u;
            worklist.push_back(FunctionSeed{t, fn.mode, ""});
        }
        // Drain any mode-switching seeds discovered during this
        // function's walk (BX Rm to a tracked-constant THUMB
        // target from ARM code, and vice versa).
        for (const auto& ms : mode_switch_seeds_) {
            uint64_t k = visit_key(ms.addr, ms.mode);
            origin_mask[k] |= 2u;
            worklist.push_back(ms);
        }
        mode_switch_seeds_.clear();
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
    // Set end_addr to next function start when adjacent (approximate
    // boundary). The last function keeps its decode-terminated
    // end_addr.
    for (std::size_t i = 0; i + 1 < functions_.size(); ++i) {
        if (functions_[i + 1].addr > functions_[i].addr) {
            functions_[i].end_addr = std::min(
                functions_[i].end_addr,
                functions_[i + 1].addr);
        }
    }

    // Stats
    stats_.functions_total = functions_.size();
    for (const auto& fn : functions_) {
        if (fn.mode == CpuMode::Thumb) ++stats_.functions_thumb;
        else                            ++stats_.functions_arm;
    }
    // Walk-source breakdown. origin_mask: 1=seed, 2=walk, 3=both.
    for (const auto& kv : origin_mask) {
        switch (kv.second) {
            case 1: ++stats_.manual_seeds_only;     break;
            case 2: ++stats_.discovered_by_walk_only; break;
            case 3: ++stats_.redundant_manual;      break;
            default: break;
        }
    }
}

}  // namespace gbarecomp
