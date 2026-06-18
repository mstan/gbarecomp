// tests/codegen/sljit_test_main.cpp — L1 JIT-mode harness.
//
// The sljit counterpart to test_main.cpp. For each entry in the SAME kTestCases
// corpus the C codegen is validated against:
//   1. Run armv4t::Interpreter::step on the decoded Instr (the oracle).
//   2. If the sljit emitter SUPPORTS the shape: JIT it (emit_instr_sljit), run
//      the shard over the same seeded g_cpu + stub bus, and diff R0..R14 + CPSR
//      + memory + ticked cycles against the interpreter. If it DECLINES: record
//      it (precision over recall — a decline is correct, never a failure).
//
// Per PRINCIPLES.md "Honest self-healing"/"Interpreter is informative": the
// interpreter is the oracle; any divergence among SUPPORTED cases is a real
// emitter bug to fix in arm_sljit.cpp — never here, never in the interpreter.
// A "supported but fn==null" or an emitter mis-compile fails the run.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "arm_decode.h"
#include "arm_ir.h"
#include "arm_sljit.h"
#include "bus.h"
#include "cpu_state.h"
#include "interpreter.h"
#include "runtime_arm.h"
#include "stubs.h"
#include "test_cases.h"
#include "thumb_decode.h"

namespace {

// ── Flat bus mirroring stubs.cpp's singleton for the interpreter side ──
struct FlatBus : armv4t::Bus {
    std::vector<uint8_t> mem;
    uint32_t base = 0;
    FlatBus(std::size_t size, uint32_t base_addr) : mem(size, 0), base(base_addr) {}
    bool in(uint32_t addr, std::size_t w) {
        if (addr < base) return false;
        std::size_t off = addr - base;
        return off + w <= mem.size();
    }
    uint8_t read8(uint32_t a) override { return in(a, 1) ? mem[a - base] : 0; }
    uint16_t read16(uint32_t a) override {
        if (!in(a, 2)) return 0;
        const uint8_t* p = &mem[a - base];
        return uint16_t(p[0]) | uint16_t(p[1]) << 8;
    }
    uint32_t read32(uint32_t a) override {
        if (!in(a, 4)) return 0;
        const uint8_t* p = &mem[a - base];
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
               uint32_t(p[3]) << 24;
    }
    void write8(uint32_t a, uint8_t v) override { if (in(a, 1)) mem[a - base] = v; }
    void write16(uint32_t a, uint16_t v) override {
        if (!in(a, 2)) return;
        uint8_t* p = &mem[a - base];
        p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
    }
    void write32(uint32_t a, uint32_t v) override {
        if (!in(a, 4)) return;
        uint8_t* p = &mem[a - base];
        p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
        p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
    }
};

uint32_t pack_cpsr(const armv4t::CPSR& c) {
    uint32_t r = c.mode & 0x1Fu;
    if (c.t) r |= (1u << 5);
    if (c.f) r |= (1u << 6);
    if (c.i) r |= (1u << 7);
    if (c.v) r |= (1u << 28);
    if (c.c) r |= (1u << 29);
    if (c.z) r |= (1u << 30);
    if (c.n) r |= (1u << 31);
    return r;
}
armv4t::CPSR unpack_cpsr(uint32_t w) {
    armv4t::CPSR c{};
    c.mode = static_cast<uint8_t>(w & 0x1Fu);
    c.t = (w >> 5) & 1u; c.f = (w >> 6) & 1u; c.i = (w >> 7) & 1u;
    c.v = (w >> 28) & 1u; c.c = (w >> 29) & 1u; c.z = (w >> 30) & 1u;
    c.n = (w >> 31) & 1u;
    return c;
}

armv4t::Instr decode_one(const TestCase& tc) {
    if (tc.thumb)
        return armv4t::ThumbDecoder::decode(uint16_t(tc.word & 0xFFFFu), tc.pc);
    return armv4t::ArmDecoder::decode(tc.word, tc.pc);
}

struct Diff { bool failed = false; std::string msg; };
void note(Diff& d, const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (!d.msg.empty()) d.msg += "\n    ";
    d.msg += buf; d.failed = true;
}

void diff_state(const TestCase& tc, const armv4t::CPUState& ci,
                const FlatBus& bi, Diff& d) {
    for (int r = 0; r < 15; ++r) {
        if (g_cpu.R[r] != ci.R[r])
            note(d, "R[%d]: interp=0x%08X jit=0x%08X", r, ci.R[r], g_cpu.R[r]);
    }
    if (tc.branches && g_cpu.R[15] != ci.R[15])
        note(d, "R[15]: interp=0x%08X jit=0x%08X", ci.R[15], g_cpu.R[15]);
    uint32_t cpsr_i = pack_cpsr(ci.cpsr);
    if (g_cpu.cpsr != cpsr_i)
        note(d, "CPSR: interp=0x%08X jit=0x%08X (diff=0x%08X)", cpsr_i,
             g_cpu.cpsr, cpsr_i ^ g_cpu.cpsr);
    if (bi.mem.size() == codegen_test::bus_size()) {
        const uint8_t* a = bi.mem.data();
        const uint8_t* b = codegen_test::bus_data();
        for (std::size_t i = 0; i < bi.mem.size(); ++i) {
            if (a[i] != b[i]) {
                note(d, "mem[0x%08X]: interp=0x%02X jit=0x%02X",
                     uint32_t(i + bi.base), a[i], b[i]);
                break;
            }
        }
    }
}

}  // namespace

int main() {
    std::printf("sljit_codegen_tests: %zu cases\n", kTestCasesCount);

    int failures = 0, supported = 0, declined = 0, skipped = 0;

    for (std::size_t idx = 0; idx < kTestCasesCount; ++idx) {
        const TestCase& tc = kTestCases[idx];
        armv4t::Instr ins = decode_one(tc);

        // ── Interpreter oracle ──
        armv4t::CPUState ci{};
        ci.cpsr = unpack_cpsr(tc.cpsr_init);
        ci.thumb = ci.cpsr.t;
        for (int r = 0; r < 16; ++r) ci.R[r] = tc.r_init[r];
        ci.R[15] = tc.pc;
        FlatBus bi(tc.mem_size ? tc.mem_size : 64u * 1024u,
                   tc.mem_size ? tc.mem_base : 0u);
        for (std::size_t k = 0; k < tc.mem_init_count; ++k)
            bi.write32(tc.mem_init[k].addr, tc.mem_init[k].value);
        uint32_t saved_pc = ci.R[15];
        uint32_t interp_cycles = 0;
        auto rr = armv4t::Interpreter::step(ci, bi, ins, &interp_cycles);
        if (rr == armv4t::Interpreter::Result::NotImplemented ||
            rr == armv4t::Interpreter::Result::Undefined) {
            ++skipped;  // interp can't model it → not an sljit concern
            continue;
        }
        if (!tc.branches) ci.R[15] = saved_pc;

        // ── Emitter decision ──
        if (!armv4t::sljit_supports(ins)) {
            ++declined;
            continue;
        }
        ++supported;

        // ── Seed the shared g_cpu + stub bus, then JIT + run ──
        std::memset(&g_cpu, 0, sizeof(g_cpu));
        for (int r = 0; r < 16; ++r) g_cpu.R[r] = tc.r_init[r];
        g_cpu.cpsr = tc.cpsr_init;
        g_cpu.R[15] = tc.pc;
        codegen_test::bus_reset(tc.mem_size ? tc.mem_base : 0u,
                                tc.mem_size ? tc.mem_size : 64u * 1024u);
        for (std::size_t k = 0; k < tc.mem_init_count; ++k)
            codegen_test::bus_write_u32_direct(tc.mem_init[k].addr,
                                               tc.mem_init[k].value);

        armv4t::SljitFn f = armv4t::emit_instr_sljit(ins);
        if (!f.fn) {
            std::printf("FAIL [%zu] %s: sljit_supports==true but emitter "
                        "declined (fn==null)\n", idx, tc.name);
            ++failures;
            continue;
        }
        f.fn();
        armv4t::free_sljit_fn(f);

        Diff d;
        diff_state(tc, ci, bi, d);
        if (codegen_test::g_ticked_cycles != interp_cycles)
            note(d, "cycles: interp=%u jit=%llu", interp_cycles,
                 (unsigned long long)codegen_test::g_ticked_cycles);
        if (d.failed) {
            std::printf("FAIL [%zu] %s\n    %s\n", idx, tc.name, d.msg.c_str());
            ++failures;
        }
    }

    std::printf("sljit_codegen_tests: %d supported & clean, %d declined, "
                "%d interp-skipped, %d FAILED\n",
                supported - failures, declined, skipped, failures);
    if (failures) return 1;
    if (supported == 0) {
        std::printf("sljit_codegen_tests: WARNING — 0 supported cases in the "
                    "corpus; the emitter validated nothing.\n");
        return 2;
    }
    std::printf("sljit_codegen_tests: all %d supported cases match the "
                "interpreter (incl. cycles)\n", supported);
    return 0;
}
