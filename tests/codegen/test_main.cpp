// tests/codegen/test_main.cpp — the L1 codegen test runner.
//
// For each entry in kTestCases:
//   1. Build an armv4t::CPUState + a per-test FlatBus from the
//      snapshot. Run armv4t::Interpreter::step on the decoded Instr.
//   2. Mirror the same snapshot into g_cpu and the singleton stub
//      bus. Invoke kTestFns[i]() (the generated recompiled function).
//   3. Diff: R[0..14] + (R[15] when the case is a branch) + CPSR +
//      memory + branch-side-effects (dispatch target, SWI imm).
//
// Any divergence is a real codegen bug to fix in arm_codegen.cpp.
// Per PRINCIPLES.md "Interpreter is informative, never load-bearing":
// the interpreter is the semantic reference. If the diff fails the
// FIX goes in the recompiler, never in the interpreter or in the
// generated code.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "arm_decode.h"
#include "arm_ir.h"
#include "bus.h"
#include "cpu_state.h"
#include "interpreter.h"
#include "runtime_arm.h"
#include "stubs.h"
#include "test_cases.h"
#include "thumb_decode.h"

namespace {

// ── Bus that mirrors stubs.cpp's singleton for the interpreter side
// of the diff. The two buses are seeded identically per-test, and
// the runner compares their contents at the end.
struct FlatBus : armv4t::Bus {
    std::vector<uint8_t> mem;
    uint32_t base = 0;
    bool oob = false;

    FlatBus(std::size_t size, uint32_t base_addr)
        : mem(size, 0), base(base_addr) {}

    bool in(uint32_t addr, std::size_t w) {
        if (addr < base) return false;
        std::size_t off = addr - base;
        return off + w <= mem.size();
    }

    uint8_t read8(uint32_t addr) override {
        if (!in(addr, 1)) { oob = true; return 0; }
        return mem[addr - base];
    }
    uint16_t read16(uint32_t addr) override {
        if (!in(addr, 2)) { oob = true; return 0; }
        const uint8_t* p = &mem[addr - base];
        return static_cast<uint16_t>(p[0]) |
               (static_cast<uint16_t>(p[1]) << 8);
    }
    uint32_t read32(uint32_t addr) override {
        if (!in(addr, 4)) { oob = true; return 0; }
        const uint8_t* p = &mem[addr - base];
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
    void write8(uint32_t addr, uint8_t v) override {
        if (!in(addr, 1)) { oob = true; return; }
        mem[addr - base] = v;
    }
    void write16(uint32_t addr, uint16_t v) override {
        if (!in(addr, 2)) { oob = true; return; }
        uint8_t* p = &mem[addr - base];
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
    void write32(uint32_t addr, uint32_t v) override {
        if (!in(addr, 4)) { oob = true; return; }
        uint8_t* p = &mem[addr - base];
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    }
};

// ── CPSR pack / unpack between interpreter-style and recomp-style ──
// The interpreter uses CPSR-as-struct (bitfields), while runtime_arm
// uses the packed uint32_t form.

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
    c.t = (w >> 5) & 1u;
    c.f = (w >> 6) & 1u;
    c.i = (w >> 7) & 1u;
    c.v = (w >> 28) & 1u;
    c.c = (w >> 29) & 1u;
    c.z = (w >> 30) & 1u;
    c.n = (w >> 31) & 1u;
    return c;
}

// ── Setup helpers ──────────────────────────────────────────────────

armv4t::Instr decode_one(const TestCase& tc) {
    if (tc.thumb) {
        return armv4t::ThumbDecoder::decode(
            static_cast<uint16_t>(tc.word & 0xFFFFu), tc.pc);
    }
    return armv4t::ArmDecoder::decode(tc.word, tc.pc);
}

// Compute the singleton bus region: union of all mem_init addresses,
// rounded out generously. For simplicity we just give every test a
// 64 KB window from 0 — every test case's working set fits.
struct BusGeom { uint32_t base; uint32_t size; };
BusGeom default_bus_geom() { return {0u, 64u * 1024u}; }

// ── Diff machinery ─────────────────────────────────────────────────

struct Diff {
    bool failed = false;
    std::string msg;
};

void note(Diff& d, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!d.msg.empty()) d.msg += "\n    ";
    d.msg += buf;
    d.failed = true;
}

void diff_state(const TestCase& tc,
                const armv4t::CPUState& cpu_interp,
                const FlatBus& bus_interp,
                Diff& d) {
    // R[0..14]: always diff.
    for (int r = 0; r < 15; ++r) {
        if (g_cpu.R[r] != cpu_interp.R[r]) {
            note(d, "R[%d]: interp=0x%08X recomp=0x%08X",
                 r, cpu_interp.R[r], g_cpu.R[r]);
        }
    }
    // R[15]: only diff when the instruction branches. For non-
    // branch ops the interpreter has already advanced R[15] by
    // 4/2 while the recomp leaves it alone.
    if (tc.branches) {
        if (g_cpu.R[15] != cpu_interp.R[15]) {
            note(d, "R[15] (branch): interp=0x%08X recomp=0x%08X",
                 cpu_interp.R[15], g_cpu.R[15]);
        }
    }
    // CPSR: interpreter uses struct; pack and diff.
    uint32_t cpsr_interp = pack_cpsr(cpu_interp.cpsr);
    if (g_cpu.cpsr != cpsr_interp) {
        note(d, "CPSR: interp=0x%08X recomp=0x%08X (diff=0x%08X)",
             cpsr_interp, g_cpu.cpsr, cpsr_interp ^ g_cpu.cpsr);
    }
    // Memory: word-grained byte-equality over the entire bus
    // window. The two buses are seeded identically and the same
    // size; mismatches mean the recomp wrote something different.
    if (bus_interp.mem.size() != codegen_test::bus_size()) {
        note(d, "bus size mismatch: interp=%zu recomp=%u",
             bus_interp.mem.size(), codegen_test::bus_size());
    } else {
        const uint8_t* a = bus_interp.mem.data();
        const uint8_t* b = codegen_test::bus_data();
        std::size_t n = bus_interp.mem.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (a[i] != b[i]) {
                note(d,
                    "mem[0x%08X]: interp=0x%02X recomp=0x%02X",
                    static_cast<uint32_t>(i + bus_interp.base),
                    a[i], b[i]);
                // Cap noise: only print the first 4 diffs.
                int extras = 0;
                for (std::size_t j = i + 1; j < n; ++j) {
                    if (a[j] != b[j]) ++extras;
                }
                if (extras) {
                    note(d, "+ %d more memory bytes differ", extras);
                }
                break;
            }
        }
    }
}

// ── Main per-case runner ───────────────────────────────────────────

bool run_case(const TestCase& tc, std::size_t idx) {
    BusGeom geom = default_bus_geom();
    if (tc.mem_size) {
        geom = BusGeom{tc.mem_base, tc.mem_size};
    }

    // ── Interpreter path ─────────────────────────────────────────
    armv4t::Instr ins = decode_one(tc);

    armv4t::CPUState cpu_interp{};
    cpu_interp.cpsr = unpack_cpsr(tc.cpsr_init);
    cpu_interp.thumb = cpu_interp.cpsr.t;
    for (int r = 0; r < 16; ++r) cpu_interp.R[r] = tc.r_init[r];
    cpu_interp.R[15] = tc.pc;  // PC starts at the instruction's own addr

    FlatBus bus_interp(geom.size, geom.base);
    for (std::size_t k = 0; k < tc.mem_init_count; ++k) {
        bus_interp.write32(tc.mem_init[k].addr, tc.mem_init[k].value);
    }

    uint32_t saved_pc = cpu_interp.R[15];
    auto r = armv4t::Interpreter::step(cpu_interp, bus_interp, ins);
    if (r == armv4t::Interpreter::Result::NotImplemented) {
        std::printf("FAIL [%zu] %s: interpreter NotImplemented for "
                    "this instruction shape — fix the interpreter or "
                    "drop the case.\n", idx, tc.name);
        return false;
    }
    if (r == armv4t::Interpreter::Result::Undefined) {
        std::printf("FAIL [%zu] %s: interpreter Undefined.\n",
                    idx, tc.name);
        return false;
    }
    // For non-branch cases, restore PC so we diff against the
    // recomp's "PC unchanged" convention.
    if (!tc.branches) {
        cpu_interp.R[15] = saved_pc;
    }

    // ── Recomp path ──────────────────────────────────────────────
    std::memset(&g_cpu, 0, sizeof(g_cpu));
    for (int rg = 0; rg < 16; ++rg) g_cpu.R[rg] = tc.r_init[rg];
    g_cpu.cpsr = tc.cpsr_init;
    g_cpu.R[15] = tc.pc;  // matches the interpreter side

    codegen_test::bus_reset(geom.base, geom.size);
    for (std::size_t k = 0; k < tc.mem_init_count; ++k) {
        codegen_test::bus_write_u32_direct(
            tc.mem_init[k].addr, tc.mem_init[k].value);
    }

    kTestFns[idx]();

    // Check for unimplemented op aborts.
    if (codegen_test::g_unimplemented_called) {
        std::printf("FAIL [%zu] %s: recomp hit runtime_unimplemented_op"
                    " op=%s pc=0x%08X — add lowering in arm_codegen.cpp\n",
                    idx, tc.name,
                    codegen_test::g_unimplemented_op
                        ? codegen_test::g_unimplemented_op : "(null)",
                    codegen_test::g_unimplemented_pc);
        return false;
    }

    // For non-branch cases the recomp doesn't touch R[15]; we
    // already restored cpu_interp.R[15], so the diff is direct.
    Diff d;
    diff_state(tc, cpu_interp, bus_interp, d);

    if (d.failed) {
        std::printf("FAIL [%zu] %s\n    %s\n",
                    idx, tc.name, d.msg.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main() {
    std::printf("codegen_tests: %zu cases\n", kTestCasesCount);
    if (kTestCasesCount != kTestFnsCount) {
        std::printf("FAIL: corpus size (%zu) != generated fn count (%u)\n",
                    kTestCasesCount, kTestFnsCount);
        return 2;
    }

    int failures = 0;
    for (std::size_t i = 0; i < kTestCasesCount; ++i) {
        if (!run_case(kTestCases[i], i)) ++failures;
    }
    if (failures) {
        std::printf("\ncodegen_tests: %d / %zu failed\n",
                    failures, kTestCasesCount);
        return 1;
    }
    std::printf("codegen_tests: all %zu cases passed\n", kTestCasesCount);
    return 0;
}
