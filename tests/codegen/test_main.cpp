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
#include "overlay_abi.h"
#include "runtime_arm.h"
#include "stubs.h"
#include "test_cases.h"
#include "thumb_decode.h"

extern "C" int overlay_runtime_trace_gate_compile_smoke(
    const GbaOverlayCallbacks* callbacks);

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

void set_env_var(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unset_env_var(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

struct EnvSnapshot {
    struct Entry {
        const char* name;
        bool present;
        std::string value;
    };
    std::vector<Entry> entries;

    explicit EnvSnapshot(const char* const* names, std::size_t count) {
        entries.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const char* value = std::getenv(names[i]);
            entries.push_back({names[i], value != nullptr,
                               value ? std::string(value) : std::string()});
        }
    }

    ~EnvSnapshot() {
        for (const Entry& e : entries) {
            if (e.present) {
                set_env_var(e.name, e.value.c_str());
            } else {
                unset_env_var(e.name);
            }
        }
        runtime_trace_reset();
    }
};

constexpr const char* kTraceEnvVars[] = {
    "GBARECOMP_RUNTIME_TRACE",
    "GBARECOMP_TRACE_ON_DISPATCH_MISS",
    "GBARECOMP_ABORT_ON_BIOS_WRITE",
    "GBARECOMP_ABORT_ON_MEM_WRITE_ADDR",
    "GBARECOMP_ABORT_ON_MEM_READ_HIGH",
    "GBARECOMP_ABORT_AFTER_BRANCHES",
    "GBARECOMP_ABORT_ON_BRANCH_PC",
};

void clear_trace_env() {
    for (const char* name : kTraceEnvVars) unset_env_var(name);
}

void seed_trace_state() {
    std::memset(&g_cpu, 0, sizeof(g_cpu));
    for (uint32_t i = 0; i < 16; ++i) {
        g_cpu.R[i] = 0x10000000u + i * 0x1111111u;
    }
    g_cpu.cpsr = CPSR_N_BIT | CPSR_C_BIT | 0x1Fu;
    g_runtime_cycles = 0x123456789ull;
}

bool cpu_and_cycles_match(const ArmCpuState& cpu_before,
                          unsigned long long cycles_before) {
    return std::memcmp(&g_cpu, &cpu_before, sizeof(g_cpu)) == 0 &&
           g_runtime_cycles == cycles_before;
}

bool trace_event_preserves_guest_state(const char* label) {
    ArmCpuState cpu_before = g_cpu;
    unsigned long long cycles_before = g_runtime_cycles;
    runtime_trace_event(RUNTIME_TRACE_BRANCH, 0x08000100u, 0x08000200u,
                        0xA5A55A5Au, 0x1234u);
    if (!cpu_and_cycles_match(cpu_before, cycles_before)) {
        std::printf("FAIL runtime_trace_gate: %s mutated guest state\n",
                    label);
        return false;
    }
    return true;
}

bool expect_trace_state(const char* label, bool enabled) {
    if ((g_runtime_trace_enabled != 0u) != enabled) {
        std::printf("FAIL runtime_trace_gate: %s enabled=%u expected=%u\n",
                    label, g_runtime_trace_enabled, enabled ? 1u : 0u);
        return false;
    }
    return true;
}

bool expect_empty_trace(const char* label) {
    RuntimeTraceEntry entries[1] = {};
    uint32_t count = runtime_trace_copy_recent(entries, 1);
    if (count != 0u) {
        std::printf("FAIL runtime_trace_gate: %s copied %u entries\n",
                    label, count);
        return false;
    }
    return true;
}

bool run_runtime_trace_gate_cases() {
    EnvSnapshot env(kTraceEnvVars,
                    sizeof(kTraceEnvVars) / sizeof(kTraceEnvVars[0]));
    clear_trace_env();

    runtime_trace_reset();
    if (!expect_trace_state("default env", false)) return false;
    seed_trace_state();
    if (!trace_event_preserves_guest_state("disabled trace")) return false;
    if (!expect_empty_trace("disabled trace")) return false;

    set_env_var("GBARECOMP_RUNTIME_TRACE", "");
    runtime_trace_reset();
    if (!expect_trace_state("empty runtime trace env", false)) return false;

    set_env_var("GBARECOMP_RUNTIME_TRACE", "0");
    runtime_trace_reset();
    if (!expect_trace_state("runtime trace env 0", false)) return false;

    set_env_var("GBARECOMP_RUNTIME_TRACE", "1");
    runtime_trace_reset();
    if (!expect_trace_state("runtime trace env 1", true)) return false;
    seed_trace_state();
    if (!trace_event_preserves_guest_state("enabled trace")) return false;
    RuntimeTraceEntry entries[1] = {};
    uint32_t count = runtime_trace_copy_recent(entries, 1);
    if (count != 1u || entries[0].kind != RUNTIME_TRACE_BRANCH ||
        entries[0].pc != 0x08000100u ||
        entries[0].cycles != 0x123456789ull) {
        std::printf("FAIL runtime_trace_gate: enabled trace entry mismatch "
                    "count=%u kind=%u pc=0x%08X cycles=%llu\n",
                    count, entries[0].kind, entries[0].pc,
                    entries[0].cycles);
        return false;
    }

    unset_env_var("GBARECOMP_RUNTIME_TRACE");
    runtime_trace_reset();
    if (!expect_trace_state("reset disables after env clear", false)) {
        return false;
    }
    if (!expect_empty_trace("reset clears trace ring")) return false;

    set_env_var("GBARECOMP_ABORT_AFTER_BRANCHES", "0");
    runtime_trace_reset();
    if (!expect_trace_state("numeric branch-count watchpoint", true)) {
        return false;
    }

    unset_env_var("GBARECOMP_ABORT_AFTER_BRANCHES");
    set_env_var("GBARECOMP_ABORT_ON_MEM_WRITE_ADDR", "0");
    runtime_trace_reset();
    if (!expect_trace_state("numeric mem-write watchpoint", true)) {
        return false;
    }

    unset_env_var("GBARECOMP_ABORT_ON_MEM_WRITE_ADDR");
    set_env_var("GBARECOMP_ABORT_ON_BRANCH_PC", "0");
    runtime_trace_reset();
    if (!expect_trace_state("numeric branch-PC watchpoint", true)) {
        return false;
    }

    unset_env_var("GBARECOMP_ABORT_ON_BRANCH_PC");
    set_env_var("GBARECOMP_ABORT_ON_BIOS_WRITE", "0");
    runtime_trace_reset();
    if (!expect_trace_state("existing BIOS watchpoint presence", true)) {
        return false;
    }

    unset_env_var("GBARECOMP_ABORT_ON_BIOS_WRITE");
    set_env_var("GBARECOMP_ABORT_ON_MEM_READ_HIGH", "0");
    runtime_trace_reset();
    if (!expect_trace_state("existing mem-read-high presence", true)) {
        return false;
    }

    return true;
}

bool run_overlay_runtime_trace_gate_cases() {
    unsigned trace_enabled = 0;
    GbaOverlayCallbacks callbacks = {};
    callbacks.runtime_trace_enabled = &trace_enabled;

    if (overlay_runtime_trace_gate_compile_smoke(&callbacks) != 0) {
        std::printf("FAIL overlay_runtime_trace_gate: disabled pointer read\n");
        return false;
    }
    trace_enabled = 1;
    if (overlay_runtime_trace_gate_compile_smoke(&callbacks) != 1) {
        std::printf("FAIL overlay_runtime_trace_gate: enabled pointer read\n");
        return false;
    }
    return true;
}

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
    uint32_t interp_cycles = 0;
    auto r = armv4t::Interpreter::step(cpu_interp, bus_interp, ins,
                                       &interp_cycles);
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

    // Cycle-cost parity. The recomp's total runtime_tick() cycles for
    // this instruction must equal the interpreter's per-instruction
    // count. Both sides use armv4t::Bus::access_cycles == 1, so this
    // validates the fixed base cost, the register-shift and PC-write
    // surcharges, the multiply operand waits, and LDM/STM access counts;
    // region-specific waitstates live in gba::GbaBus and are checked by
    // the differential oracle, not here.
    if (codegen_test::g_ticked_cycles != interp_cycles) {
        note(d, "cycles: interp=%u recomp=%llu", interp_cycles,
             static_cast<unsigned long long>(codegen_test::g_ticked_cycles));
    }
    // runtime_tick can synchronously deliver an IRQ. BX must therefore expose
    // the destination instruction-set mode before ticking, not merely before
    // dispatching the destination.
    if (ins.op == armv4t::IrOp::BX &&
        codegen_test::g_cpsr_at_first_tick != UINT32_MAX) {
        constexpr uint32_t kT = 1u << 5;
        if ((codegen_test::g_cpsr_at_first_tick & kT) !=
            (cpu_interp.cpsr.t ? kT : 0u)) {
            note(d, "BX mode at tick: expected T=%u, observed cpsr=0x%08X",
                 cpu_interp.cpsr.t ? 1u : 0u,
                 codegen_test::g_cpsr_at_first_tick);
        }
    }

    if (d.failed) {
        std::printf("FAIL [%zu] %s\n    %s\n",
                    idx, tc.name, d.msg.c_str());
        return false;
    }
    return true;
}

bool run_call_return_stack_cases() {
    runtime_init(nullptr);

    runtime_call_push_return(0x00000100u);
    runtime_call_push_return(0x00000200u);
    if (!runtime_call_should_return(0x00000101u)) {
        std::printf("FAIL runtime_call_return_stack: non-local return "
                    "did not match older frame\n");
        runtime_shutdown();
        return false;
    }
    if (runtime_call_should_return(0x00000200u)) {
        std::printf("FAIL runtime_call_return_stack: non-local return "
                    "left younger frame active\n");
        runtime_shutdown();
        return false;
    }

    runtime_call_push_return(0x00000300u);
    if (!runtime_call_should_return(0x00000301u)) {
        std::printf("FAIL runtime_call_return_stack: top-frame return "
                    "did not ignore THUMB bit\n");
        runtime_shutdown();
        return false;
    }

    runtime_shutdown();
    return true;
}

int thumb_alu_imm_test_override(uint32_t pc, uint32_t original,
                                uint32_t* out) {
    if (pc == 0x0800E2D2u && original == 12u) {
        *out = 15u;
        return 1;
    }
    if (pc == 0x080B291Cu && original == 240u) {
        *out = 360u;
        return 1;
    }
    return 0;
}

int thumb_alu_imm_accept_everything(uint32_t, uint32_t, uint32_t* out) {
    *out = 99u;
    return 1;
}

bool run_thumb_alu_immediate_override_cases() {
    std::size_t sub_idx = kTestCasesCount;
    std::size_t add_idx = kTestCasesCount;
    std::size_t unconfigured_idx = kTestCasesCount;
    std::size_t arm_cmp_idx = kTestCasesCount;
    for (std::size_t i = 0; i < kTestCasesCount; ++i) {
        if (std::strcmp(kTestCases[i].name,
                        "thumb_sub_imm_override_fixture") == 0) {
            sub_idx = i;
        } else if (std::strcmp(kTestCases[i].name,
                               "thumb_add_imm_override_fixture") == 0) {
            add_idx = i;
        } else if (std::strcmp(kTestCases[i].name, "thumb_mov_imm") == 0) {
            unconfigured_idx = i;
        } else if (std::strcmp(kTestCases[i].name,
                               "arm_cmp_imm_override_fixture") == 0) {
            arm_cmp_idx = i;
        }
    }
    if (sub_idx == kTestCasesCount || add_idx == kTestCasesCount ||
        unconfigured_idx == kTestCasesCount ||
        arm_cmp_idx == kTestCasesCount) {
        std::printf("FAIL thumb_alu_immediate_override: fixture missing\n");
        return false;
    }

    // The same exact-PC chokepoint supports ARM rotated immediates. This is
    // the form used by Minish Cap's IWRAM sprite renderer.
    std::memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.R[2] = 300u;
    g_cpu.cpsr = 0x1Fu;
    g_runtime_thumb_alu_imm_override = thumb_alu_imm_test_override;
    kTestFns[arm_cmp_idx]();
    if (cpsr_n() != 1u || cpsr_z() != 0u || cpsr_c() != 0u ||
        cpsr_v() != 0u) {
        std::printf("FAIL alu_immediate_override: ARM accepted flags "
                    "cpsr=0x%08X\n", g_cpu.cpsr);
        g_runtime_thumb_alu_imm_override = nullptr;
        return false;
    }

    codegen_test::bus_reset(0u, 64u * 1024u);
    std::memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.R[1] = 100u;
    g_cpu.cpsr = (1u << 5) | 0x1Fu;
    g_runtime_thumb_alu_imm_override = thumb_alu_imm_test_override;
    kTestFns[sub_idx]();
    if (g_cpu.R[1] != 85u || cpsr_c() != 1u || cpsr_n() != 0u ||
        cpsr_z() != 0u || cpsr_v() != 0u) {
        std::printf("FAIL thumb_alu_immediate_override: accepted value/flags "
                    "r1=%u cpsr=0x%08X\n", g_cpu.R[1], g_cpu.cpsr);
        g_runtime_thumb_alu_imm_override = nullptr;
        return false;
    }

    // The callback rejects this other exact PC, so the original immediate
    // remains in force.
    std::memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.R[5] = 100u;
    g_cpu.cpsr = (1u << 5) | 0x1Fu;
    kTestFns[add_idx]();
    if (g_cpu.R[5] != 112u) {
        std::printf("FAIL thumb_alu_immediate_override: rejected site changed "
                    "r5=%u\n", g_cpu.R[5]);
        g_runtime_thumb_alu_imm_override = nullptr;
        return false;
    }

    // Even an accepting callback cannot affect an ordinary THUMB immediate:
    // the generator omitted the chokepoint because PC 0x100 was not opted in.
    std::memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.cpsr = (1u << 5) | 0x1Fu;
    g_runtime_thumb_alu_imm_override = thumb_alu_imm_accept_everything;
    kTestFns[unconfigured_idx]();
    if (g_cpu.R[0] != 5u) {
        std::printf("FAIL thumb_alu_immediate_override: unconfigured site "
                    "changed r0=%u\n", g_cpu.R[0]);
        g_runtime_thumb_alu_imm_override = nullptr;
        return false;
    }

    // Null is the faithful fast path.
    g_runtime_thumb_alu_imm_override = nullptr;
    std::memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.R[1] = 100u;
    g_cpu.cpsr = (1u << 5) | 0x1Fu;
    kTestFns[sub_idx]();
    if (g_cpu.R[1] != 88u) {
        std::printf("FAIL thumb_alu_immediate_override: null path r1=%u\n",
                    g_cpu.R[1]);
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
    if (!run_call_return_stack_cases()) ++failures;
    if (!run_thumb_alu_immediate_override_cases()) ++failures;
    if (!run_runtime_trace_gate_cases()) ++failures;
    if (!run_overlay_runtime_trace_gate_cases()) ++failures;
    if (failures) {
        std::printf("\ncodegen_tests: %d / %zu failed\n",
                    failures, kTestCasesCount);
        return 1;
    }
    std::printf("codegen_tests: all %zu cases passed\n", kTestCasesCount);
    return 0;
}
