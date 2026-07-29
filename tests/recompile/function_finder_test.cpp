#include "function_finder.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr std::uint32_t kBase = 0x08000000u;

void put16(std::vector<std::uint8_t>& rom, std::size_t off,
           std::uint16_t value) {
    rom[off] = static_cast<std::uint8_t>(value);
    rom[off + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& rom, std::size_t off,
           std::uint32_t value) {
    put16(rom, off, static_cast<std::uint16_t>(value));
    put16(rom, off + 2, static_cast<std::uint16_t>(value >> 16));
}

bool has_function(const gbarecomp::FunctionFinder& finder,
                  std::uint32_t addr, gbarecomp::CpuMode mode) {
    for (const auto& fn : finder.functions()) {
        if (fn.addr == addr && fn.mode == mode) return true;
    }
    return false;
}

const gbarecomp::Function* find_function(
    const gbarecomp::FunctionFinder& finder,
    std::uint32_t addr,
    gbarecomp::CpuMode mode) {
    for (const auto& fn : finder.functions()) {
        if (fn.addr == addr && fn.mode == mode) return &fn;
    }
    return nullptr;
}

}  // namespace

int main() {
    std::vector<std::uint8_t> rom(0x80, 0);

    // ldr r1,[pc,#8]  -> tracker sees the even ROM data pointer below
    // pop {r1}        -> r1 is overwritten from the runtime stack
    // bx r1           -> dynamic return; must not reuse the stale literal
    put16(rom, 0x00, 0x4902u);
    put16(rom, 0x02, 0xBC02u);
    put16(rom, 0x04, 0x4708u);
    put32(rom, 0x0C, kBase + 0x20u);

    // Make the false target valid-looking ARM code. Before the regression
    // fix, the stale constant caused this address to be emitted.
    put32(rom, 0x20, 0xE12FFF1Eu);  // bx lr

    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    finder.add_seed({kBase, gbarecomp::CpuMode::Thumb, "pop_bx_return"});
    finder.run(32);

    if (!has_function(finder, kBase, gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr, "entry function was not discovered\n");
        return 1;
    }
    if (has_function(finder, kBase + 0x20u, gbarecomp::CpuMode::Arm)) {
        std::fprintf(stderr,
                     "POP destination retained a stale constant and seeded "
                     "0x%08X as ARM code\n",
                     kBase + 0x20u);
        return 1;
    }

    // A stack-local helper may be copied again at an overlapping address.
    // The explicit source address must win over the first fixed mapping, and
    // CFG roots from the older placement must not truncate the newer body.
    std::vector<std::uint8_t> relocated_rom(0x80, 0);
    constexpr std::size_t kTemplate = 0x40;
    put16(relocated_rom, kTemplate + 0x00, 0x2800u); // cmp r0,#0
    put16(relocated_rom, kTemplate + 0x02, 0xD001u); // beq +8
    put16(relocated_rom, kTemplate + 0x04, 0x4770u); // bx lr
    put16(relocated_rom, kTemplate + 0x06, 0x46C0u); // nop
    put16(relocated_rom, kTemplate + 0x08, 0x4770u); // bx lr

    constexpr std::uint32_t kRam = 0x03000000u;
    gbarecomp::FunctionFinder relocated(
        relocated_rom.data(), relocated_rom.size(), kBase);
    relocated.set_speculative_literal_harvest(false);
    relocated.add_code_copy(kRam, kBase + kTemplate, 0x20, "first copy");
    relocated.add_seed({kRam, gbarecomp::CpuMode::Thumb, "first"});
    gbarecomp::FunctionSeed overlapping{
        kRam + 4, gbarecomp::CpuMode::Thumb, "overlapping"};
    overlapping.source_addr = kBase + kTemplate;
    relocated.add_seed(overlapping);
    relocated.run(32);

    const auto* overlap_fn = find_function(
        relocated, kRam + 4, gbarecomp::CpuMode::Thumb);
    if (!overlap_fn || overlap_fn->source_addr != kBase + kTemplate) {
        std::fprintf(stderr,
                     "overlapping relocation did not retain explicit ROM backing\n");
        return 1;
    }
    if (overlap_fn->end_addr <= kRam + 8) {
        std::fprintf(stderr,
                     "older placement incorrectly truncated overlapping CFG\n");
        return 1;
    }
    const auto* overlap_target = find_function(
        relocated, kRam + 12, gbarecomp::CpuMode::Thumb);
    if (!overlap_target ||
        overlap_target->source_addr != kBase + kTemplate + 8) {
        std::fprintf(stderr,
                     "relocation bias did not propagate to direct branch target\n");
        return 1;
    }

    // A bounded AOT sweep finds compiler-shaped prologues without runtime
    // evidence, then uses two strong entries in an even-address THUMB pointer
    // table to infer the table mode and recover a leaf function with no PUSH.
    std::vector<std::uint8_t> aot_rom(0x100, 0);
    put16(aot_rom, 0x00, 0x4802u);  // ldr r0,[pc,#8] -> table base
    put16(aot_rom, 0x02, 0x4770u);  // bx lr
    put32(aot_rom, 0x0C, kBase + 0xC0u);
    put16(aot_rom, 0x40, 0xB500u);  // push {lr}
    put16(aot_rom, 0x42, 0x46C0u);
    put16(aot_rom, 0x44, 0x46C0u);
    put16(aot_rom, 0x46, 0x46C0u);
    put16(aot_rom, 0x48, 0x46C0u);
    put16(aot_rom, 0x4A, 0xBD00u);  // pop {pc}
    put16(aot_rom, 0x50, 0xB500u);  // second strong table-mode vote
    put16(aot_rom, 0x52, 0x46C0u);
    put16(aot_rom, 0x54, 0x46C0u);
    put16(aot_rom, 0x56, 0x46C0u);
    put16(aot_rom, 0x58, 0x46C0u);
    put16(aot_rom, 0x5A, 0xBD00u);
    put16(aot_rom, 0x60, 0x2001u);  // movs r0,#1 (leaf, no prologue)
    put16(aot_rom, 0x62, 0x46C0u);
    put16(aot_rom, 0x64, 0x46C0u);
    put16(aot_rom, 0x66, 0x46C0u);
    put16(aot_rom, 0x68, 0x46C0u);
    put16(aot_rom, 0x6A, 0x4770u);  // bx lr
    put32(aot_rom, 0xC0, kBase + 0x40u);
    put32(aot_rom, 0xC4, kBase + 0x50u);
    put32(aot_rom, 0xC8, kBase + 0x60u);

    gbarecomp::FunctionFinder aot(aot_rom.data(), aot_rom.size(), kBase);
    aot.set_speculative_literal_harvest(false);
    aot.set_aot_scan_range(kBase + 0x40u, kBase + 0x80u);
    aot.add_seed({kBase, gbarecomp::CpuMode::Thumb, "entry"});
    aot.run(64);
    if (!has_function(aot, kBase + 0x40u, gbarecomp::CpuMode::Thumb) ||
        !has_function(aot, kBase + 0x50u, gbarecomp::CpuMode::Thumb) ||
        !has_function(aot, kBase + 0x60u, gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "bounded AOT scan missed a prologue or address-taken "
                     "THUMB leaf\n");
        return 1;
    }
    if (aot.stats().aot_prologue_seeds != 2 ||
        aot.stats().aot_pointer_tables != 1 ||
        aot.stats().aot_pointer_seeds != 1) {
        std::fprintf(stderr,
                     "bounded AOT scan stats disagree: prologues=%zu "
                     "tables=%zu leaves=%zu\n",
                     aot.stats().aot_prologue_seeds,
                     aot.stats().aot_pointer_tables,
                     aot.stats().aot_pointer_seeds);
        return 1;
    }

    // A copied ARM blob can contain internal callable helpers that are not
    // reachable from its declared entry. Scan the ROM backing of a known
    // code-copy range for stack-frame prologues, including leaf helpers that
    // save callee-saved registers without saving LR.
    std::vector<std::uint8_t> copied_rom(0x100, 0);
    constexpr std::size_t kCopiedTemplate = 0x40;
    put32(copied_rom, kCopiedTemplate + 0x00, 0xE12FFF1Eu); // bx lr
    put32(copied_rom, kCopiedTemplate + 0x1C, 0xE1A0C00Du); // mov ip,sp
    put32(copied_rom, kCopiedTemplate + 0x20, 0xE92D0030u); // push {r4,r5}
    put32(copied_rom, kCopiedTemplate + 0x24, 0xE1A00000u); // nop
    put32(copied_rom, kCopiedTemplate + 0x28, 0xE1A00000u);
    put32(copied_rom, kCopiedTemplate + 0x2C, 0xE1A00000u);
    put32(copied_rom, kCopiedTemplate + 0x30, 0xE1A00000u);
    put32(copied_rom, kCopiedTemplate + 0x34, 0xE8BD0030u); // pop {r4,r5}
    put32(copied_rom, kCopiedTemplate + 0x38, 0xE12FFF1Eu); // bx lr

    constexpr std::uint32_t kCopiedRam = 0x03001000u;
    gbarecomp::FunctionFinder copied(
        copied_rom.data(), copied_rom.size(), kBase);
    copied.set_speculative_literal_harvest(false);
    copied.set_static_resume_all(true);
    copied.add_code_copy(
        kCopiedRam, kBase + kCopiedTemplate, 0x40, "ARM helper blob");
    copied.add_seed(
        {kCopiedRam, gbarecomp::CpuMode::Arm, "copied_entry"});
    copied.run(32);
    const auto* copied_helper = find_function(
        copied, kCopiedRam + 0x1Cu, gbarecomp::CpuMode::Arm);
    bool copied_push_resume = false;
    if (copied_helper) {
        for (std::uint32_t pc : copied_helper->alias_entries) {
            if (pc == kCopiedRam + 0x20u) copied_push_resume = true;
        }
    }
    if (!copied_helper ||
        copied_helper->source_addr != kBase + kCopiedTemplate + 0x1Cu ||
        copied.stats().aot_code_copy_seeds != 1 ||
        !copied_push_resume) {
        std::fprintf(stderr,
                     "code-copy scan missed internal ARM helper: fn=%s "
                     "source=0x%08X seeds=%zu push_resume=%s\n",
                     copied_helper ? "yes" : "no",
                     copied_helper ? copied_helper->source_addr : 0u,
                     copied.stats().aot_code_copy_seeds,
                     copied_push_resume ? "yes" : "no");
        return 1;
    }

    std::printf("function_finder_tests: PASS\n");
    return 0;
}
