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

uint32_t FunctionFinder::read_u32(uint32_t addr) const {
    std::size_t off = addr - rom_base_;
    return  static_cast<uint32_t>(rom_[off])
         | (static_cast<uint32_t>(rom_[off + 1]) << 8)
         | (static_cast<uint32_t>(rom_[off + 2]) << 16)
         | (static_cast<uint32_t>(rom_[off + 3]) << 24);
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

    while (count++ < kMaxInstrs && addr_in_rom(pc) &&
           (pc - rom_base_ + step) <= rom_size_) {
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
            fn.direct_branch_targets.push_back(ins.branch_target);
            ++stats_.branch_targets_discovered;
        }

        // Indirect = computed; we can't follow statically.
        if (ins.is_indirect) {
            fn.has_indirect_transfer = true;
            ++stats_.indirect_transfer_count;
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
    // Seed worklist. BFS by index so we process roots first
    // (entry_point + tmc seeds in TSV order), then branch targets
    // discovered along the way.
    std::vector<FunctionSeed> worklist = seeds_;

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
            worklist.push_back(FunctionSeed{t, fn.mode, ""});
        }
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
}

}  // namespace gbarecomp
