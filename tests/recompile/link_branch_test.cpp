// Far-branch BL lowering: a THUMB/ARM BL whose link value is dead at the
// target is a long jump (MKSC re-enters its per-frame main loop heads with
// `bl`). It must still write LR but must not push a call-return frame —
// under suspended multiplayer dispatch those frames were never consumed and
// aborted paired sessions after 1024 frames.
#include "emit_function.h"
#include "function_finder.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t kBase = 0x08000000u;
int failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL link_branch: %s\n", what);
    ++failures;
}

struct Rom {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x1000, 0);
    void h(std::uint32_t pc, std::uint16_t v) {
        bytes[pc - kBase] = static_cast<std::uint8_t>(v);
        bytes[pc - kBase + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    void w(std::uint32_t pc, std::uint32_t v) {
        h(pc, static_cast<std::uint16_t>(v));
        h(pc + 2, static_cast<std::uint16_t>(v >> 16));
    }
    void thumb_bl(std::uint32_t pc, std::uint32_t target) {
        const std::int32_t off = static_cast<std::int32_t>(target - (pc + 4u));
        h(pc, static_cast<std::uint16_t>(0xF000u | ((off >> 12) & 0x7FF)));
        h(pc + 2, static_cast<std::uint16_t>(0xF800u | ((off >> 1) & 0x7FF)));
    }
    void arm_bl(std::uint32_t pc, std::uint32_t target) {
        const std::int32_t off = static_cast<std::int32_t>(target - (pc + 8u));
        w(pc, 0xEB000000u | ((static_cast<std::uint32_t>(off) >> 2) & 0xFFFFFFu));
    }
    bool dead(std::uint32_t target, bool thumb) const {
        return gbarecomp::link_register_dead_at(bytes.data(), bytes.size(),
                                                kBase, target, thumb);
    }
};

bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    Rom rom;
    // THUMB targets.
    const std::uint32_t loop_head = 0x08000100u, push_lr = 0x08000200u,
        split_read = 0x08000300u, split_dead = 0x08000400u, leaf = 0x08000500u,
        spin = 0x08000600u, mov_to_lr = 0x08000700u;
    rom.thumb_bl(loop_head, leaf);             // MKSC shape: bl VBlankWait
    rom.h(push_lr, 0xB500u);                   // push {lr}
    rom.h(split_read, 0x2800u);                // cmp r0,#0
    rom.h(split_read + 2, 0xD001u);            // beq +2 insns
    rom.thumb_bl(split_read + 4, leaf);        // fallthrough: LR dead
    rom.h(split_read + 8, 0x4670u);            // taken: mov r0, lr (read)
    rom.h(split_dead, 0x2800u);                // cmp r0,#0
    rom.h(split_dead + 2, 0xD001u);            // beq +2 insns
    rom.thumb_bl(split_dead + 4, leaf);        // fallthrough: LR dead
    rom.h(split_dead + 8, 0xE7FEu);            // taken: b . (never reads)
    rom.h(leaf, 0x4770u);                      // bx lr
    rom.h(spin, 0x46C0u);                      // nop
    rom.h(spin + 2, 0xE7FDu);                  // b spin
    rom.h(mov_to_lr, 0x46C0u);                 // nop
    rom.h(mov_to_lr + 2, 0x46B6u);             // mov lr, r6

    check(rom.dead(loop_head, true), "BL-first loop head is a far branch");
    check(!rom.dead(push_lr, true), "push {lr} is a call");
    check(!rom.dead(split_read, true), "one path reading LR keeps the call");
    check(rom.dead(split_dead, true), "both paths clobbering LR is dead");
    check(!rom.dead(leaf, true), "bx lr is a call");
    check(rom.dead(spin, true), "LR-free infinite loop never observes LR");
    check(rom.dead(mov_to_lr, true), "mov lr, rN clobbers LR");
    check(!rom.dead(0x03000000u, true), "RAM targets stay calls");
    check(!rom.dead(0x08000FFEu + 0x10u, true), "out-of-image targets stay calls");

    // ARM: a BL at the target clobbers LR; stmfd sp!, {lr} reads it.
    const std::uint32_t arm_head = 0x08000800u, arm_push = 0x08000900u;
    rom.arm_bl(arm_head, 0x08000A00u);
    rom.w(arm_push, 0xE92D4000u);              // stmfd sp!, {lr}
    check(rom.dead(arm_head, false), "ARM BL-first head is a far branch");
    check(!rom.dead(arm_push, false), "ARM push {lr} is a call");

    // Emission: the far-branch BL writes LR and branches; the real call
    // keeps its call-return frame.
    rom.thumb_bl(kBase + 0x0u, loop_head);
    rom.thumb_bl(kBase + 0x4u, push_lr);
    rom.h(kBase + 0x8u, 0x4770u);
    gbarecomp::Function fn{};
    fn.addr = kBase;
    fn.source_addr = kBase;
    fn.mode = gbarecomp::CpuMode::Thumb;
    fn.name = "t";
    fn.end_addr = kBase + 0xAu;
    const std::unordered_map<std::uint64_t, std::string> names;
    const std::string body = gbarecomp::emit_function_body_str(
        fn, rom.bytes.data(), rom.bytes.size(), kBase, names);
    check(!contains(body, "runtime_call_push_return(0x08000004u)"),
          "far-branch BL pushed a call-return frame");
    check(!contains(body, "runtime_call_cancel_return(0x08000004u)"),
          "far-branch BL kept call fallthrough");
    check(contains(body, "g_cpu.R[14] = 0x08000005u;"),
          "far-branch BL must still write LR");
    check(contains(body, "GBARECOMP_TAIL_DISPATCH(0x08000100u);"),
          "far-branch BL must transfer to its target");
    check(contains(body, "runtime_call_push_return(0x08000008u)"),
          "real call lost its call-return frame");

    // Relocated bodies keep call semantics.
    gbarecomp::Function moved = fn;
    moved.addr = 0x03000000u;
    moved.end_addr = 0x0300000Au;
    const std::string moved_body = gbarecomp::emit_function_body_str(
        moved, rom.bytes.data(), rom.bytes.size(), kBase, names);
    check(contains(moved_body, "runtime_call_push_return(0x03000004u)"),
          "relocated BL must stay a call");

    if (failures) return 1;
    std::printf("link_branch_tests: OK\n");
    return 0;
}
