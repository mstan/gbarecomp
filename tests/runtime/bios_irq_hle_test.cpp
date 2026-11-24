// SPDX-License-Identifier: MPL-2.0
#include "bios_hle.h"
#include "gba_bios.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "native_program.h"
#include "runtime_bus_bridge.h"
#include "runtime_context.h"

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {

constexpr uint32_t kHandler = 0x08000000u;
constexpr uint32_t kInterrupted = 0x08001000u;
constexpr uint32_t kIrqStack = 0x03007fa0u;
constexpr uint32_t kUserStack = 0x03007f00u;
constexpr uint32_t kIrqI = 0x80u;
constexpr uint32_t kCallbackLatch = 0x4b534951u;
constexpr uint32_t kResumeLatch = 0x5149524bu;
unsigned calls;
bool exercise_nested;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void handler() {
    ++calls;
    require(g_cpu.R[0] == 0x04000000u && g_cpu.R[14] == 0x138u,
            "firmware callback register contract");
    require((g_cpu.cpsr & 0xffu) == 0x92u, "IRQ callback must start masked in ARM");
    require(bus_read_u32(0x00u) == kCallbackLatch, "callback BIOS prefetch");
    if (calls == 1) {
        require(g_cpu.R[13] == kIrqStack - 24u, "IRQ stack size");
        require(bus_read_u32(g_cpu.R[13]) == 0x1234u, "saved r0 on IRQ stack");
        require(bus_read_u32(g_cpu.R[13] + 20u) == kInterrupted + 4u,
                "saved return address on IRQ stack");
        if (exercise_nested) {
            const auto parent_psr = g_cpu.banked_spsr[ARM_BANK_IRQ];
            const uint32_t parent_lr = g_cpu.R[14];
            g_cpu.cpsr &= ~kIrqI;
            runtime_irq(kHandler + 4u);
            require(g_cpu.R[15] == kHandler + 4u && g_cpu.R[13] == kIrqStack - 24u,
                    "nested IRQ restores its own caller");
            g_cpu.cpsr |= kIrqI;
            g_cpu.banked_spsr[ARM_BANK_IRQ] = parent_psr;
            g_cpu.R[14] = parent_lr;
        }
    } else {
        require(g_cpu.R[13] == kIrqStack - 48u, "nested IRQ stack frame");
    }
    g_cpu.R[0] = 0;
    ++g_cpu.R[4];
    g_cpu.R[15] = g_cpu.R[14];
}

}  // namespace

int main() try {
    auto bus = std::make_unique<gba::GbaBus>();
    auto ppu = std::make_unique<gba::GbaPpu>();
    gba::GbaBios firmware;
    std::array<uint8_t, gba::GbaBios::kSize> bytes{};
    for (unsigned i = 0; i < 4; ++i) {
        bytes[0x13cu + i] = uint8_t(kCallbackLatch >> (8u * i));
        bytes[0x144u + i] = uint8_t(kResumeLatch >> (8u * i));
    }
    std::string error;
    require(firmware.load_from_bytes(bytes.data(), bytes.size(), "", &error),
            "synthetic BIOS prefetch fixture");
    bus->set_bios(&firmware);
    gbarecomp::set_active_bus(bus.get());
    gbarecomp::set_active_ppu(ppu.get());
    runtime_init(bus.get());
    const DispatchEntry entry{kHandler, 0, 0, handler};
    const RuntimeNativeProgram program{&entry, 1, nullptr, 0, nullptr, 0};
    runtime_select_native_program(&program);
    bus->write32(0x03007ffcu, kHandler);
    gba::bios_hle_set_irq_enabled(true);
    require(g_bios_hle_dispatch_hook(0x18u, 0) == 0, "ordinary call cannot start an IRQ");
    for (bool nested : {false, true}) {
        exercise_nested = nested;
        calls = 0;
        g_cpu = {};
        g_cpu.cpsr = 0x3fu;
        g_cpu.R[0] = 0x1234u;
        g_cpu.R[4] = 10u;
        g_cpu.R[13] = kUserStack;
        g_cpu.R[15] = kInterrupted;
        g_cpu.banked_sp[ARM_BANK_IRQ] = kIrqStack;
        const uint64_t before = g_runtime_cycles;
        runtime_irq(kInterrupted);
        require(calls == (nested ? 2u : 1u), "native cartridge callback count");
        require(g_cpu.cpsr == 0x3fu && g_cpu.R[15] == kInterrupted &&
                    g_cpu.R[13] == kUserStack && g_cpu.R[0] == 0x1234u,
                "return restores Thumb caller and volatile registers");
        require(g_cpu.R[4] == 10u + calls, "cartridge owns nonvolatile registers");
        require(g_cpu.banked_sp[ARM_BANK_IRQ] == kIrqStack, "IRQ stack unwinds");
        require(bus_read_u32(0u) == kResumeLatch, "caller BIOS prefetch");
        const uint64_t regular = 28u + runtime_refill_cycles(kHandler, 0) +
            runtime_refill_cycles(kInterrupted, 1);
        const uint64_t inner = 28u + runtime_refill_cycles(kHandler, 0) +
            runtime_refill_cycles(kHandler + 4u, 0);
        require(g_runtime_cycles - before == regular + (nested ? inner : 0u),
                "wrapper charges stack transfers and both pipeline refills");
    }
    gbarecomp::RuntimeArmContext context;
    gbarecomp::runtime_capture_arm_context(context);
    gba::bios_hle_set_irq_enabled(false);
    require(!g_bios_hle_dispatch_hook, "disabled HLE keeps original dispatch");
    gbarecomp::runtime_restore_arm_context(context);
    require(g_bios_hle_dispatch_hook == context.bios_dispatch_hook,
            "BIOS dispatch selection follows the bound CPU context");
    gba::bios_hle_set_irq_enabled(false);
    runtime_select_native_program(nullptr);
    runtime_shutdown();
    gbarecomp::set_active_ppu(nullptr);
    gbarecomp::set_active_bus(nullptr);
    std::puts("Native IRQ callback, nested return, timing and BIOS prefetch passed");
} catch (const std::exception& error) {
    std::fprintf(stderr, "Native IRQ check: %s\n", error.what());
    return 1;
}
