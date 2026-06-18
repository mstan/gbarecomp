// tests/sljit/block_test.cpp — whole-function composition harness (P5 step 1).
//
// Validates that the per-instruction sljit leaves chain correctly inside ONE
// host function (emit_block_sljit): register + memory state flows through g_cpu
// between instructions, and per-instruction cycles accumulate. For each
// synthesized instruction sequence we run the interpreter step-by-step and the
// JIT'd block from the identical seed, then diff R0..R14 + CPSR + memory +
// total ticked cycles. (R15 is excluded — the straight-line block does not yet
// maintain it; the faithful prologue lands in the next P5 step.)
//
// Both sides execute the SAME decoded instructions, so a sequence validates
// JIT==interp regardless of hand-assembly intent. A sequence containing an
// unsupported shape is skipped (reported), never a failure.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "arm_decode.h"
#include "arm_ir.h"
#include "arm_sljit.h"
#include "bus.h"
#include "cpu_state.h"
#include "interpreter.h"
#include "runtime_arm.h"
#include "stubs.h"

namespace {

struct FlatBus : armv4t::Bus {
    std::vector<uint8_t> mem;
    FlatBus() : mem(64u * 1024u, 0) {}
    bool in(uint32_t a, std::size_t w) { return a + w <= mem.size(); }
    uint8_t read8(uint32_t a) override { return in(a, 1) ? mem[a] : 0; }
    uint16_t read16(uint32_t a) override {
        if (!in(a, 2)) return 0;
        return uint16_t(mem[a]) | uint16_t(mem[a + 1]) << 8;
    }
    uint32_t read32(uint32_t a) override {
        if (!in(a, 4)) return 0;
        return uint32_t(mem[a]) | uint32_t(mem[a + 1]) << 8 |
               uint32_t(mem[a + 2]) << 16 | uint32_t(mem[a + 3]) << 24;
    }
    void write8(uint32_t a, uint8_t v) override { if (in(a, 1)) mem[a] = v; }
    void write16(uint32_t a, uint16_t v) override {
        if (!in(a, 2)) return;
        mem[a] = uint8_t(v); mem[a + 1] = uint8_t(v >> 8);
    }
    void write32(uint32_t a, uint32_t v) override {
        if (!in(a, 4)) return;
        mem[a] = uint8_t(v); mem[a + 1] = uint8_t(v >> 8);
        mem[a + 2] = uint8_t(v >> 16); mem[a + 3] = uint8_t(v >> 24);
    }
};

uint32_t pack_cpsr(const armv4t::CPSR& c) {
    uint32_t r = c.mode & 0x1Fu;
    if (c.t) r |= 1u << 5;
    if (c.f) r |= 1u << 6;
    if (c.i) r |= 1u << 7;
    if (c.v) r |= 1u << 28;
    if (c.c) r |= 1u << 29;
    if (c.z) r |= 1u << 30;
    if (c.n) r |= 1u << 31;
    return r;
}

struct Seq {
    const char* name;
    std::vector<uint32_t> words;
    uint32_t r_init[16];
};

constexpr uint32_t kBase = 0x08000000u;

bool run_seq(const Seq& s) {
    std::vector<armv4t::Instr> prog;
    prog.reserve(s.words.size());
    for (std::size_t i = 0; i < s.words.size(); ++i)
        prog.push_back(armv4t::ArmDecoder::decode(
            s.words[i], kBase + uint32_t(i) * 4u));

    for (const auto& in : prog) {
        if (!armv4t::sljit_supports(in)) {
            std::printf("  SKIP %s: contains an unsupported shape (op=%s)\n",
                        s.name, armv4t::ir_op_name(in.op));
            return true;  // skip, not a failure
        }
    }

    // ── Interpreter oracle ──
    armv4t::CPUState ci{};
    for (int r = 0; r < 16; ++r) ci.R[r] = s.r_init[r];
    FlatBus bi;
    uint32_t interp_cycles = 0;
    for (const auto& in : prog) {
        ci.R[15] = in.pc;
        uint32_t c = 0;
        auto rr = armv4t::Interpreter::step(ci, bi, in, &c);
        if (rr == armv4t::Interpreter::Result::NotImplemented ||
            rr == armv4t::Interpreter::Result::Undefined) {
            std::printf("  SKIP %s: interpreter cannot model an instruction\n", s.name);
            return true;
        }
        interp_cycles += c;
    }

    // ── JIT block ──
    std::memset(&g_cpu, 0, sizeof(g_cpu));
    for (int r = 0; r < 16; ++r) g_cpu.R[r] = s.r_init[r];
    codegen_test::bus_reset(0u, 64u * 1024u);

    armv4t::SljitFn f = armv4t::emit_block_sljit(prog.data(),
                                                 unsigned(prog.size()));
    if (!f.fn) {
        std::printf("  FAIL %s: all instructions supported but block declined\n", s.name);
        return false;
    }
    f.fn();
    armv4t::free_sljit_fn(f);

    // ── Diff (R0..R14 + CPSR + memory + cycles; R15 excluded) ──
    int diffs = 0;
    for (int r = 0; r < 15; ++r) {
        if (g_cpu.R[r] != ci.R[r]) {
            std::printf("  FAIL %s: R[%d] interp=0x%08X jit=0x%08X\n",
                        s.name, r, ci.R[r], g_cpu.R[r]);
            ++diffs;
        }
    }
    uint32_t cpsr_i = pack_cpsr(ci.cpsr);
    if (g_cpu.cpsr != cpsr_i) {
        std::printf("  FAIL %s: CPSR interp=0x%08X jit=0x%08X\n",
                    s.name, cpsr_i, g_cpu.cpsr);
        ++diffs;
    }
    if (bi.mem.size() == codegen_test::bus_size()) {
        const uint8_t* b = codegen_test::bus_data();
        for (std::size_t i = 0; i < bi.mem.size(); ++i) {
            if (bi.mem[i] != b[i]) {
                std::printf("  FAIL %s: mem[0x%08X] interp=0x%02X jit=0x%02X\n",
                            s.name, uint32_t(i), bi.mem[i], b[i]);
                ++diffs;
                break;
            }
        }
    }
    if (codegen_test::g_ticked_cycles != interp_cycles) {
        std::printf("  FAIL %s: cycles interp=%u jit=%llu\n", s.name,
                    interp_cycles,
                    (unsigned long long)codegen_test::g_ticked_cycles);
        ++diffs;
    }
    if (diffs == 0)
        std::printf("  OK   %s (%zu instrs, %u cycles)\n", s.name,
                    prog.size(), interp_cycles);
    return diffs == 0;
}

}  // namespace

int main() {
    // Hand-assembled ARM sequences. Both interp and JIT run the same decoded
    // instructions, so these validate composition regardless of intent.
    const Seq seqs[] = {
        {"alu_chain", {
            0xE3A00010u,  // mov r0, #0x10
            0xE3A01020u,  // mov r1, #0x20
            0xE0802001u,  // add r2, r0, r1
            0xE38230FFu,  // orr r3, r2, #0xFF
            0xE2434001u,  // sub r4, r3, #1
        }, {0}},
        {"shift_chain", {
            0xE3A00001u,        // mov r0, #1
            0xE1A01200u,        // mov r1, r0, lsl #4   (=0x10)
            0xE1A02121u,        // mov r2, r1, lsr #2   (=0x04)
            0xE0813002u,        // add r3, r1, r2       (=0x14)
        }, {0}},
        {"mem_roundtrip", {
            0xE3A030ABu,  // mov r3, #0xAB
            0xE5843000u,  // str r3, [r4]
            0xE5945000u,  // ldr r5, [r4]
            0xE5C43004u,  // strb r3, [r4, #4]
        }, {0, 0, 0, 0, 0x100u, 0}},
        {"setflags_chain", {
            0xE3B00000u,  // movs r0, #0       (Z=1)
            0xE2900001u,  // adds r0, r0, #1   (Z=0)
            0xE2500001u,  // subs r0, r0, #1   (Z=1, C=1)
        }, {0}},
    };

    int fails = 0, n = int(sizeof(seqs) / sizeof(seqs[0]));
    std::printf("sljit_block_tests: %d sequences\n", n);
    for (const auto& s : seqs) if (!run_seq(s)) ++fails;

    if (fails) {
        std::printf("sljit_block_tests: %d / %d sequences FAILED\n", fails, n);
        return 1;
    }
    std::printf("sljit_block_tests: all sequences composed correctly "
                "(JIT block == interpreter)\n");
    return 0;
}
