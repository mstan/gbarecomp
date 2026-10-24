#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "runtime_context.h"
#include <memory>
#include <cstdio>

extern "C" uint32_t g_irq_nest_depth;
extern "C" uint32_t g_irq_iret_depth;

int main() {
    auto bus = std::make_unique<gba::GbaBus>();
    auto ppu = std::make_unique<gba::GbaPpu>();
    bus->io().set_bus(bus.get()); bus->io().set_ppu(ppu.get());
    // An interpreted IRQ return resumes inside a native routine. Its first
    // instruction must be left for the waiting native frame, not run twice.
    bus->write32(0x03000000u, 0xE25EF004u); // subs pc, lr, #4
    bus->write32(0x03000010u, 0xE3A0007Fu); // mov r0, #127
    bus->write32(0x03000014u, 0xEAFFFFFEu); // b .
    gbarecomp::set_active_bus(bus.get());
    gbarecomp::set_active_ppu(ppu.get());
    runtime_init(bus.get());
    g_cpu = {};
    g_cpu.cpsr = 0x92u;
    g_cpu.R[0] = 11;
    g_cpu.R[14] = 0x03000014u;
    g_cpu.R[15] = 0x03000000u;
    g_cpu.banked_spsr[ARM_BANK_IRQ] = 0x1fu;
    g_irq_nest_depth = 1; g_irq_iret_depth = 0;
    const int result = runtime_bridge_interpret(0x03000000u, false, 0, 16);
    const bool ok = result == 1 && g_cpu.R[15] == 0x03000010u &&
                    g_cpu.R[0] == 11 && g_irq_iret_depth == 1;
    std::printf("bridged IRQ return: result=%d pc=%08X r0=%u iret=%u\n",
                result, g_cpu.R[15], g_cpu.R[0], g_irq_iret_depth);
    g_irq_nest_depth = g_irq_iret_depth = 0;
    gbarecomp::set_active_bus(nullptr); gbarecomp::set_active_ppu(nullptr);
    runtime_shutdown();
    return ok ? 0 : 1;
}
