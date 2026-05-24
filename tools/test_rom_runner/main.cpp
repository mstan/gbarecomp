// test_rom_runner — direct-boot a recompiled jsmolka gba-suite test
// ROM and report pass/fail.
//
// Convention (per gba-suite README + lib/macros.inc):
//   r12 holds the result code at the m_test_eval point:
//     r12 == 0  -> all tests passed
//     r12 == N  -> first failed test was test #N
//
// Each test sets r12 to its own number BEFORE running, then on
// failure does `b eval` (leaving r12 as the failing number). On
// success it falls through to the next test (which overwrites r12
// with its own number). Reaching `eval:` naturally only happens if
// every test's pass-branch fired, so r12 traces the last-attempted
// test.
//
// The `eval:` block calls `m_vsync` which busy-loops on
// DISPSTAT.VBlank. Our runtime doesn't pump the PPU, so execution
// gets stuck in that loop AFTER the tests have already written
// their final r12. So we just step for a budget and then read r12
// — the value is stable by the time the loop appears.
//
// We direct-boot (no BIOS): set PC=0x080000C0 (cart entry), SP to
// a post-BIOS-like address, and dispatch the cart's recompiled
// `start_vector` function. The cart ROM is mapped into bus memory
// so the test code can read its own literal pools.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

namespace {

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    if (sz <= 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(sz));
    in.read(reinterpret_cast<char*>(out.data()), sz);
    return in.good();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: test_rom_runner <rom.gba> [--steps N]\n"
            "\n"
            "Direct-boots <rom.gba> (skips BIOS), steps the recompiled\n"
            "ROM, reports r12 at exit.\n"
            "  r12 == 0  -> all tests passed\n"
            "  r12 == N  -> failed at test #N\n");
        return 2;
    }
    const std::string rom_path = argv[1];
    int steps_budget = 5'000'000;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--steps" && i + 1 < argc) {
            steps_budget = std::atoi(argv[++i]);
        }
    }

    std::vector<uint8_t> rom;
    if (!read_file(rom_path, rom)) {
        std::fprintf(stderr, "test_rom_runner: cannot read %s\n",
                     rom_path.c_str());
        return 2;
    }

    gba::GbaBus bus;
    gba::GbaPpu ppu;
    bus.set_rom(rom.data(), rom.size());
    bus.io().set_ppu(&ppu);
    bus.io().set_bus(&bus);

    gbarecomp::set_active_bus(&bus);
    runtime_init(&bus);

    // Inline reset (reset_recomp_cpu is internal to runtime.cpp;
    // we don't run run_game(), so we replicate its body here).
    for (int i = 0; i < 16; ++i) g_cpu.R[i] = 0;

    // Direct boot: skip BIOS, jump to cart entry. The test ROM's
    // entry point at 0x080000C0 is a B instruction to `main:`.
    // Stack pointer mimics post-BIOS state (BIOS would have
    // initialized banked SPs; we approximate the user-mode SP).
    g_cpu.R[15] = 0x080000C0u;
    g_cpu.R[13] = 0x03007F00u;     // user SP, post-BIOS-ish
    g_cpu.cpsr  = CPSR_I_BIT | CPSR_F_BIT | 0x13u;  // SVC, I/F masked

    // Step loop. Each runtime_dispatch executes one recompiled
    // function. The function may itself call sub-functions (direct
    // C calls via the names_by_addr map) — those don't count as
    // separate dispatches. We also detect "stuck PC" (same value
    // for K consecutive dispatches) as a sign the m_vsync busy
    // loop has been entered, at which point r12 is stable.
    uint32_t prev_pc = ~0u;
    int stuck_count = 0;
    constexpr int kStuckThreshold = 256;
    int steps_taken = 0;
    for (int i = 0; i < steps_budget; ++i) {
        const uint32_t pc = g_cpu.R[15];
        runtime_dispatch(pc);
        ++steps_taken;
        if (g_cpu.R[15] == prev_pc) {
            if (++stuck_count >= kStuckThreshold) break;
        } else {
            stuck_count = 0;
            prev_pc = g_cpu.R[15];
        }
    }

    std::printf("test_rom_runner: rom=%s\n", rom_path.c_str());
    std::printf("  steps taken:  %d / %d\n", steps_taken, steps_budget);
    std::printf("  final PC:     0x%08X\n", g_cpu.R[15]);
    std::printf("  r12 (result): %u (0x%08X)\n",
                g_cpu.R[12], g_cpu.R[12]);
    std::printf("  unmapped bus accesses:  %zu\n",
                bus.unmapped_count());
    std::printf("  unmapped io accesses:   %zu\n",
                bus.io().unmapped_count());

    if (g_cpu.R[12] == 0) {
        std::printf("  RESULT: PASS (all tests)\n");
        return 0;
    }
    std::printf("  RESULT: FAIL at test #%u\n", g_cpu.R[12]);
    return 1;
}
