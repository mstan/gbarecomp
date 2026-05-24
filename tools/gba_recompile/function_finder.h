// function_finder.h — Recompiler-side function discovery.
//
// Given the ROM bytes, an entry point, and a set of seed symbols,
// walk the CFG to find every reachable function address and its
// approximate basic-block layout. The output drives codegen.
//
// Design notes:
//   - We track ARM vs THUMB mode per address because the cart freely
//     mixes them (crt0 is ARM; most of the game is THUMB).
//   - "Function" here is "address reachable as a call target (BL,
//     BLX, or B-as-tail-call) or as a control-flow root from the
//     entry / seed list".
//   - Indirect branches (BX reg, LDR PC, ldm with PC, computed
//     calls) are recorded as "indirect dispatch needed" — the
//     runtime resolves them via the dispatch table at execution time.
//   - Literal pools (data embedded in .text via `LDR rX, =const`)
//     are NOT skipped in this first cut — function bodies stop at
//     the first unconditional terminator (B, BX, undefined). The
//     codegen will see the literal-pool bytes as "garbage" Instrs
//     and emit them as `/* literal: 0x...... */` comments.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gbarecomp {

enum class CpuMode : uint8_t {
    Arm = 0,
    Thumb = 1,
};

// A seed: an address we KNOW is a function entry, with its mode +
// (optional) name. Sources: entry_point from game.toml, imported
// tmc symbols.
struct FunctionSeed {
    uint32_t addr;
    CpuMode  mode;
    std::string name;   // empty → finder generates "func_XXXXXXXX"
};

// Output: one entry per discovered function.
struct Function {
    uint32_t addr;
    CpuMode  mode;
    std::string name;
    // Approximate end address (exclusive). Set by the finder via
    // "next function start" once discovery is done; for the last
    // function in the corpus this stays at addr + 4 (placeholder).
    uint32_t end_addr;
    // The set of direct branch targets discovered inside this body
    // (B/BL/conditional B). Useful for codegen labeling and for
    // verifying CFG coverage.
    std::vector<uint32_t> direct_branch_targets;
    // True if at least one indirect control transfer (BX reg,
    // LDR PC, ldm with PC, etc.) was decoded in this function.
    bool has_indirect_transfer = false;
};

struct FinderStats {
    std::size_t functions_total = 0;
    std::size_t functions_arm = 0;
    std::size_t functions_thumb = 0;
    std::size_t indirect_transfer_count = 0;
    std::size_t branch_targets_discovered = 0;
    std::size_t undefined_instr_count = 0;
};

class FunctionFinder {
public:
    // ROM bytes; expected to be the cartridge image as loaded into
    // memory at `rom_base` (typically 0x08000000). `rom_base` is
    // used to translate guest addresses to ROM-buffer indices.
    FunctionFinder(const uint8_t* rom_bytes, std::size_t rom_size,
                   uint32_t rom_base);

    // Add a seed. Duplicate addresses are tolerated; the first
    // seed's mode + name wins.
    void add_seed(const FunctionSeed& seed);

    // Run discovery starting from all seeds. Bounded by
    // `max_functions` to avoid runaways during bring-up.
    void run(std::size_t max_functions = 4096);

    // Sorted-by-address output.
    const std::vector<Function>& functions() const { return functions_; }
    const FinderStats& stats() const { return stats_; }

private:
    const uint8_t* rom_;
    std::size_t    rom_size_;
    uint32_t       rom_base_;

    std::vector<FunctionSeed> seeds_;
    std::vector<Function>     functions_;
    std::unordered_map<uint64_t, std::size_t> visited_;  // key=(addr<<1)|mode
    FinderStats stats_{};

    bool addr_in_rom(uint32_t addr) const {
        return addr >= rom_base_ &&
               (addr - rom_base_) < rom_size_;
    }

    uint32_t read_u32(uint32_t addr) const;
    uint16_t read_u16(uint32_t addr) const;
    void discover_one(uint32_t addr, CpuMode mode,
                      const std::string& name);
};

}  // namespace gbarecomp
