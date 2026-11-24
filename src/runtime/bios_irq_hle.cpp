// SPDX-License-Identifier: MPL-2.0
#include "bios_hle.h"

#include "gba_bus.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

#include <array>

extern "C" uint32_t g_irq_nest_depth;

namespace gba {
namespace {

constexpr uint32_t kIrqVector = 0x18u;
constexpr uint32_t kIrqReturn = 0x138u;
constexpr uint32_t kIrqHandler = 0x03fffffcu;
constexpr uint32_t kIrqMode = 0x12u;
constexpr std::array<unsigned, 6> kSavedRegisters{0, 1, 2, 3, 12, 14};
constexpr uint32_t kStackBytes = kSavedRegisters.size() * 4;

// Cartridge-facing IRQ ABI and protected BIOS reads are documented in GBATEK,
// BIOS Interrupt Handling and Reading from BIOS Memory. The cartridge owns
// interrupt acknowledgement and decides when to allow nesting.
void irq_enter(GbaBus& bus) {
    // Vector branch, then the stack save. Use the engine's existing N/S data
    // costs so a relocated IRQ stack retains its memory-region timing.
    runtime_tick(3);
    g_cpu.R[15] = 0x128u;
    const uint32_t stack = g_cpu.R[13] - kStackBytes;
    uint32_t cycles = 1;
    for (unsigned i = 0; i < kSavedRegisters.size(); ++i) {
        const uint32_t address = (stack + i * 4u) & ~3u;
        cycles += runtime_mem_cycles(address, 4, i != 0);
        bus_write_u32(address, g_cpu.R[kSavedRegisters[i]]);
    }
    g_cpu.R[13] = stack;
    runtime_tick(cycles);

    g_cpu.R[0] = 0x04000000u;
    g_cpu.R[14] = kIrqReturn;
    runtime_tick(2);
    g_cpu.R[15] = 0x134u;
    bus.latch_bios_prefetch(g_cpu.R[15], false);
    const uint32_t target = bus_read_u32(kIrqHandler) & ~3u;
    runtime_tick(4 + runtime_mem_cycles(kIrqHandler, 4, 0) +
                 runtime_refill_cycles(target, 0));
    g_cpu.R[15] = target;
}

void irq_return(GbaBus& bus) {
    const uint32_t stack = g_cpu.R[13];
    uint32_t cycles = 2;
    for (unsigned i = 0; i < kSavedRegisters.size(); ++i) {
        const uint32_t address = (stack + i * 4u) & ~3u;
        cycles += runtime_mem_cycles(address, 4, i != 0);
        g_cpu.R[kSavedRegisters[i]] = bus_read_u32(address);
    }
    g_cpu.R[13] = stack + kStackBytes;
    runtime_tick(cycles);

    g_cpu.R[15] = 0x13cu;
    bus.latch_bios_prefetch(g_cpu.R[15], false);
    const uint32_t target = g_cpu.R[14] - 4u;
    runtime_tick(3 + runtime_exception_return_refill(target));
    runtime_exception_return(target);
}

int dispatch_irq(uint32_t pc, int thumb) {
    if ((pc != kIrqVector && pc != kIrqReturn) || thumb || !g_irq_nest_depth ||
        (g_cpu.cpsr & 0x1fu) != kIrqMode || !(g_cpu.cpsr & 0x80u) ||
        !gbarecomp::native_service_window_allowed())
        return 0;
    auto* bus = gbarecomp::active_bus();
    if (!bus) return 0;
    g_runtime_resume_pc = 0;
    g_cpu.R[15] = pc;
    if (pc == kIrqVector) irq_enter(*bus);
    else irq_return(*bus);
    return 1;
}

}  // namespace

void bios_hle_set_irq_enabled(bool enabled) {
    g_bios_hle_dispatch_hook = enabled ? &dispatch_irq : nullptr;
}

}  // namespace gba
