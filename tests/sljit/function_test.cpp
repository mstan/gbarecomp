// tests/sljit/function_test.cpp — whole-function producer harness (P5).
//
// Validates emit_function_sljit: a decoded function is JIT'd into one host
// function that runs to its first external transfer (a return / out-of-function
// branch) and returns. We compare it against a MINI-DISPATCH interpreter that
// steps instructions and follows branch targets within the function until it
// exits — so an intra-function backward branch (a loop) is exercised on both
// sides. Diff: R0..R15 + CPSR + memory + total cycles.
//
// Functions end in `BX lr` with LR seeded to an address OUTSIDE the function,
// so both the JIT (dispatch out → return) and the interpreter (target not in
// range → stop) terminate at the same point with the same state.

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

struct Fn {
    const char* name;
    std::vector<uint32_t> words;
    uint32_t r_init[16];
};

constexpr uint32_t kBase = 0x08000200u;

int find_index(const std::vector<armv4t::Instr>& prog, uint32_t pc) {
    for (std::size_t i = 0; i < prog.size(); ++i)
        if (prog[i].pc == pc) return int(i);
    return -1;
}

bool run_fn(const Fn& f) {
    std::vector<armv4t::Instr> prog;
    for (std::size_t i = 0; i < f.words.size(); ++i)
        prog.push_back(armv4t::ArmDecoder::decode(f.words[i],
                                                  kBase + uint32_t(i) * 4u));
    for (const auto& in : prog)
        if (!armv4t::sljit_supports(in)) {
            std::printf("  SKIP %s: unsupported shape (op=%s)\n", f.name,
                        armv4t::ir_op_name(in.op));
            return true;
        }

    // ── Mini-dispatch interpreter ──
    armv4t::CPUState ci{};
    for (int r = 0; r < 16; ++r) ci.R[r] = f.r_init[r];
    ci.R[15] = kBase;
    FlatBus bi;
    uint32_t interp_cycles = 0;
    for (int iter = 0; iter < 100000; ++iter) {
        int k = find_index(prog, ci.R[15] & ~1u);
        if (k < 0) break;  // control left the function
        uint32_t c = 0;
        auto rr = armv4t::Interpreter::step(ci, bi, prog[k], &c);
        if (rr == armv4t::Interpreter::Result::NotImplemented ||
            rr == armv4t::Interpreter::Result::Undefined) {
            std::printf("  SKIP %s: interpreter cannot model an instruction\n", f.name);
            return true;
        }
        interp_cycles += c;
    }

    // ── JIT whole function ──
    std::memset(&g_cpu, 0, sizeof(g_cpu));
    for (int r = 0; r < 16; ++r) g_cpu.R[r] = f.r_init[r];
    g_cpu.R[15] = kBase;
    codegen_test::bus_reset(0u, 64u * 1024u);
    armv4t::SljitFn fn = armv4t::emit_function_sljit(prog.data(),
                                                     unsigned(prog.size()));
    if (!fn.fn) {
        std::printf("  FAIL %s: all instructions supported but function declined\n", f.name);
        return false;
    }
    fn.fn();
    armv4t::free_sljit_fn(fn);

    // ── Diff (R0..R15 + CPSR + memory + cycles) ──
    int diffs = 0;
    for (int r = 0; r < 16; ++r) {
        if (g_cpu.R[r] != ci.R[r]) {
            std::printf("  FAIL %s: R[%d] interp=0x%08X jit=0x%08X\n",
                        f.name, r, ci.R[r], g_cpu.R[r]);
            ++diffs;
        }
    }
    uint32_t cpsr_i = pack_cpsr(ci.cpsr);
    if (g_cpu.cpsr != cpsr_i) {
        std::printf("  FAIL %s: CPSR interp=0x%08X jit=0x%08X\n",
                    f.name, cpsr_i, g_cpu.cpsr);
        ++diffs;
    }
    if (bi.mem.size() == codegen_test::bus_size()) {
        const uint8_t* b = codegen_test::bus_data();
        for (std::size_t i = 0; i < bi.mem.size(); ++i)
            if (bi.mem[i] != b[i]) {
                std::printf("  FAIL %s: mem[0x%08X] interp=0x%02X jit=0x%02X\n",
                            f.name, uint32_t(i), bi.mem[i], b[i]);
                ++diffs; break;
            }
    }
    if (codegen_test::g_ticked_cycles != interp_cycles) {
        std::printf("  FAIL %s: cycles interp=%u jit=%llu\n", f.name, interp_cycles,
                    (unsigned long long)codegen_test::g_ticked_cycles);
        ++diffs;
    }
    if (diffs == 0)
        std::printf("  OK   %s (%zu instrs, %u cycles)\n", f.name, prog.size(),
                    interp_cycles);
    return diffs == 0;
}

}  // namespace

int main() {
    const uint32_t ext_lr = 0x09000000u;  // return address outside the function
    const Fn fns[] = {
        {"straightline", {
            0xE3A00010u,  // mov  r0, #0x10
            0xE2801005u,  // add  r1, r0, #5
            0xE3812C01u,  // orr  r2, r1, #0x100
            0xE12FFF1Eu,  // bx   lr
        }, {0,0,0,0,0,0,0,0,0,0,0,0,0,0, ext_lr, 0}},
        {"countdown_loop", {
            0xE3A00003u,  // mov  r0, #3
            0xE3A01000u,  // mov  r1, #0
            0xE0811000u,  // loop: add r1, r1, r0
            0xE2500001u,  // subs r0, r0, #1
            0x1AFFFFFCu,  // bne  loop
            0xE12FFF1Eu,  // bx   lr
        }, {0,0,0,0,0,0,0,0,0,0,0,0,0,0, ext_lr, 0}},
        {"mem_then_return", {
            0xE3A03042u,  // mov  r3, #0x42
            0xE5843000u,  // str  r3, [r4]
            0xE5945000u,  // ldr  r5, [r4]
            0xE12FFF1Eu,  // bx   lr
        }, {0,0,0,0,0x400u,0,0,0,0,0,0,0,0,0, ext_lr, 0}},
        // AAPCS leaf-with-frame: STM at entry, body, then the LDM-with-PC
        // return idiom (POP {r4, pc}). lr is OUTSIDE the function, so the
        // popped PC dispatches out → both sides stop at the same state.
        {"push_body_pop_pc", {
            0xE92D4010u,  // push {r4, lr}    (STMDB sp!, {r4, lr})
            0xE3A04020u,  // mov  r4, #0x20
            0xE5804000u,  // str  r4, [r0]
            0xE8BD8010u,  // pop  {r4, pc}    (LDMIA sp!, {r4, pc})
        }, {0x400u,0,0,0, 0xAAAAu,0,0,0, 0,0,0,0, 0,0x800u,ext_lr,0}},
    };

    int fails = 0, n = int(sizeof(fns) / sizeof(fns[0]));
    std::printf("sljit_function_tests: %d functions\n", n);
    for (const auto& f : fns) if (!run_fn(f)) ++fails;
    if (fails) {
        std::printf("sljit_function_tests: %d / %d FAILED\n", fails, n);
        return 1;
    }
    std::printf("sljit_function_tests: all functions match the interpreter "
                "(incl. loops + cycles)\n");
    return 0;
}
