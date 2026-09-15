// Reuse the codegen harness bus/miss recorders, with the real runtime helpers.
#include "runtime_arm.h"
#include "stubs.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
int failures = 0;
uint32_t hook_pc = 0, hook_swi = 0;
int hook_thumb = -1;
void check(bool condition, const char* what) {
    if (!condition) { std::printf("FAIL: %s\n", what); ++failures; }
}
void reset(bool thumb) {
    codegen_test::bus_reset(0x03000000, 0x8000);
    std::memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.cpsr = CPSR_C_BIT | 0x1fu | (thumb ? CPSR_T_BIT : 0u);
    g_cpu.R[15] = 0x08000102;
    g_bios_hle_hook = nullptr;
    g_runtime_ram_dispatch_hook = nullptr;
}
int ram_hook(uint32_t pc, int thumb) {
    hook_pc = pc; hook_thumb = thumb;
    return 1;
}
int hle_hook(uint32_t swi) { hook_swi = swi; return 1; }

void test_exchange_and_miss() {
    for (uint32_t target : {0x08000100u, 0x08000101u}) {
        reset(!(target & 1));
        runtime_dispatch_with_exchange(target);
        const auto expected = g_cpu;
        const auto miss = codegen_test::g_last_dispatch_target;
        reset(!(target & 1));
        g_runtime_tail_arg = target;
        runtime_dispatch_with_exchange_tail();
        check(codegen_test::g_dispatch_called && codegen_test::g_last_dispatch_target == miss,
              "tail exchange preserves dispatch miss target");
        check(std::memcmp(&g_cpu, &expected, sizeof(g_cpu)) == 0,
              "tail exchange matches ordinary CPU state");
        check(bool(g_cpu.cpsr & CPSR_T_BIT) == bool(target & 1),
              "exchange sets instruction mode from target bit zero");
    }
    reset(false);
    g_runtime_tail_arg = 0x08000101;
    runtime_dispatch_tail();
    check(!(g_cpu.cpsr & CPSR_T_BIT), "plain tail dispatch does not exchange instruction set");
    check(codegen_test::g_last_dispatch_target == 0x08000101,
          "plain tail miss retains original target including low bit");

    reset(true);
    g_runtime_ram_dispatch_hook = ram_hook;
    g_runtime_tail_arg = 0x02000101;
    runtime_dispatch_tail();
    check(hook_pc == 0x02000100 && hook_thumb == 1,
          "RAM hook receives aligned PC and current instruction mode");
    check(!codegen_test::g_dispatch_called, "handled RAM transfer does not fall through to miss");
    g_runtime_ram_dispatch_hook = nullptr;
}

void test_swi() {
    for (bool thumb : {false, true}) {
        const uint32_t imm = thumb ? 0x06u : 0x060000u;
        reset(thumb);
        runtime_swi(imm);
        const auto expected = g_cpu;
        reset(thumb);
        const auto saved_cpsr = g_cpu.cpsr;
        g_runtime_tail_arg = imm;
        runtime_swi_tail();
        check(std::memcmp(&g_cpu, &expected, sizeof(g_cpu)) == 0,
              "tail SWI matches ordinary exception entry including banked registers");
        check(codegen_test::g_dispatch_called && codegen_test::g_last_dispatch_target == 8,
              "unhandled SWI enters BIOS vector");
        check(g_cpu.R[14] == 0x08000102 && g_cpu.R[15] == 8,
              "SWI preserves return address in supervisor LR");
        check(g_cpu.banked_spsr[ARM_BANK_SUPERVISOR] == saved_cpsr,
              "SWI saves pre-exception CPSR");
        check(codegen_test::g_ticked_cycles == 3 &&
                  (codegen_test::g_cpsr_at_first_tick & CPSR_I_BIT),
              "SWI charges three cycles after masking interrupts");

        reset(thumb);
        g_bios_hle_hook = hle_hook;
        const auto before_hle = g_cpu;
        g_runtime_tail_arg = imm;
        runtime_swi_tail();
        check(hook_swi == 6, "HLE decodes ARM and THUMB SWI immediates");
        check(!codegen_test::g_dispatch_called && codegen_test::g_ticked_cycles == 0,
              "handled HLE does not enter BIOS or charge exception cycles");
        check(std::memcmp(&g_cpu, &before_hle, sizeof(g_cpu)) == 0,
              "handled HLE avoids supervisor mode transition");
    }
    g_bios_hle_hook = nullptr;
}
}  // namespace

int main() {
    test_exchange_and_miss();
    test_swi();
    if (failures) return 1;
    std::puts("tail dispatch: exchange, misses, RAM hooks and SWI parity passed");
    return 0;
}
