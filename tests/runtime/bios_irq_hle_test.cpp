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
constexpr uint32_t kSwiReturnLatch = 0x53574932u;
constexpr uint32_t kWaitHandler = kHandler + 8u;
constexpr uint32_t kWaitCaller = kHandler + 16u;
constexpr uint32_t kFirmwareFlags = 0x03007ff8u;
constexpr uint32_t kIe = 0x04000200u;
constexpr uint32_t kIf = 0x04000202u;
constexpr uint32_t kIme = 0x04000208u;
unsigned calls;
unsigned wait_handler_calls;
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

void wait_handler() {
    ++wait_handler_calls;
    require((g_cpu.banked_spsr[ARM_BANK_IRQ] & 0x1fu) == 0x1fu,
            "wait interrupt preempts the firmware's System-mode frame");
    const uint16_t acknowledge = bus_read_u16(kIf);
    bus_write_u16(kIf, acknowledge);
    bus_write_u16(kFirmwareFlags, bus_read_u16(kFirmwareFlags) | acknowledge);
    g_cpu.R[15] = g_cpu.R[14];
}

void check_interrupt_wait(gba::GbaBus& bus) {
    constexpr uint16_t vblank = gba::GbaIo::IrqVBlank;
    constexpr uint16_t spurious = gba::GbaIo::IrqTimer0;
    constexpr uint16_t unrelated = gba::GbaIo::IrqSerial;
    constexpr uint32_t supervisor_stack = 0x03007fe0u;
    std::array<uint8_t, 32> rom{};
    rom[16] = 5;
    rom[17] = 0xdf;
    rom[18] = 4;
    rom[19] = 0xdf;
    bus.set_rom(rom.data(), rom.size());
    bus.write32(0x03007ffcu, kWaitHandler);
    bus.write16(kFirmwareFlags, vblank | unrelated);
    bus.write16(kIe, vblank | spurious);
    bus.write8(kIme, 0);

    g_cpu = {};
    g_cpu.cpsr = 0x3fu;
    g_cpu.R[2] = 0x1234u;
    g_cpu.R[4] = 0x5678u;
    g_cpu.R[13] = kUserStack;
    g_cpu.R[14] = kInterrupted;
    g_cpu.R[15] = kWaitCaller + 2u;
    g_cpu.banked_sp[ARM_BANK_IRQ] = kIrqStack;
    g_cpu.banked_sp[ARM_BANK_SUPERVISOR] = supervisor_stack;
    runtime_swi(5);
    require(bus.io().halted() && (g_cpu.cpsr & 0x3fu) == 0x1fu &&
                g_cpu.R[13] == kUserStack - 16u,
            "VBlankIntrWait parks the System-mode service on the guest stack");
    require(bus.read16(kFirmwareFlags) == unrelated && bus.read16(kIme) == 1,
            "VBlankIntrWait discards only the old VBlank flag and enables IME");

    bus.io().request_irq(spurious);
    runtime_tick(1);
    require(!bus.io().halted(), "unrelated interrupt wakes HALT");
    runtime_dispatch(g_cpu.R[15]);
    require(bus.io().halted() && bus.read16(kFirmwareFlags) == (unrelated | spurious),
            "wait resumes sleeping without consuming an unmatched flag");

    bus.io().request_irq(vblank);
    runtime_tick(1);
    require(!bus.io().halted(), "selected interrupt wakes HALT");
    runtime_dispatch(g_cpu.R[15]);
    require(g_cpu.cpsr == 0x3fu && g_cpu.R[15] == kWaitCaller + 2u &&
                g_cpu.R[13] == kUserStack && g_cpu.R[14] == kInterrupted,
            "wait returns to its Thumb caller and unwinds its stack");
    require(g_cpu.R[2] == 0x1234u && g_cpu.R[4] == 0x5678u &&
                g_cpu.banked_sp[ARM_BANK_SUPERVISOR] == supervisor_stack &&
                bus.read16(kFirmwareFlags) == (unrelated | spurious),
            "wait preserves caller registers and unrelated wake flags");

    g_cpu.R[0] = 0;
    g_cpu.R[1] = spurious;
    g_cpu.R[15] = kWaitCaller + 4u;
    runtime_swi(4);
    require(!bus.io().halted() && g_cpu.R[15] == kWaitCaller + 4u &&
                bus.read16(kFirmwareFlags) == unrelated,
            "IntrWait can consume an already latched selected flag immediately");
}

void check_halt(gba::GbaBus& bus, bool thumb, bool master_enable, bool irq_masked) {
    constexpr uint16_t enabled = gba::GbaIo::IrqTimer0;
    constexpr uint16_t disabled = gba::GbaIo::IrqSerial;
    constexpr uint32_t supervisor_stack = 0x03007fe0u;
    std::array<uint8_t, 8> rom{};
    rom[0] = 2;
    rom[1] = 0xdf;
    rom[6] = 2;
    rom[7] = 0xef;
    bus.set_rom(rom.data(), rom.size());
    bus.write32(0x03007ffcu, kWaitHandler);
    bus.write16(kIf, 0xffffu);
    bus.write16(kIe, enabled);
    bus.write8(kIme, master_enable);
    bus.write16(kFirmwareFlags, disabled);

    g_cpu = {};
    g_cpu.cpsr = 0x4000001fu | (thumb ? 0x20u : 0u) | (irq_masked ? kIrqI : 0u);
    for (unsigned i = 0; i < 13; ++i) g_cpu.R[i] = i + 0x1234u;
    g_cpu.R[13] = kUserStack;
    g_cpu.R[14] = kInterrupted;
    g_cpu.R[15] = kHandler + (thumb ? 2u : 8u);
    g_cpu.banked_sp[ARM_BANK_IRQ] = kIrqStack;
    g_cpu.banked_sp[ARM_BANK_SUPERVISOR] = supervisor_stack;
    const auto caller = g_cpu;
    const unsigned before = wait_handler_calls;
    runtime_swi(thumb ? 2u : 2u << 16);
    require(bus.io().halted() && g_cpu.R[15] == 0x1b4u &&
                g_cpu.R[13] == kUserStack - 8u,
            "Halt parks with its interrupted System-mode frame");

    bus.io().request_irq(disabled);
    runtime_tick(1);
    require(bus.io().halted(), "a disabled interrupt does not wake Halt");
    bus.write16(kIf, disabled);
    bus.io().request_irq(enabled);
    runtime_tick(1);
    require(!bus.io().halted(), "Halt wakes even when IRQ entry is masked");
    const bool took_irq = master_enable && !irq_masked;
    require(wait_handler_calls == before + unsigned(took_irq) &&
                bus.read16(kIf) == (took_irq ? 0u : enabled),
            "Halt wake preserves the ordinary IRQ mask and acknowledgement rules");
    runtime_dispatch(g_cpu.R[15]);
    for (unsigned i = 0; i < 16; ++i)
        require(g_cpu.R[i] == caller.R[i], "Halt restores every caller register");
    require(g_cpu.cpsr == caller.cpsr &&
                g_cpu.banked_sp[ARM_BANK_SUPERVISOR] == supervisor_stack &&
                bus_read_u32(0u) == kSwiReturnLatch,
            "Halt returns with the caller CPSR, stack and BIOS prefetch");
    require(bus.read16(kFirmwareFlags) == (disabled | (took_irq ? enabled : 0u)),
            "Halt leaves the firmware wait flags for the cartridge");
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
        bytes[0x190u + i] = uint8_t(kSwiReturnLatch >> (8u * i));
    }
    std::string error;
    require(firmware.load_from_bytes(bytes.data(), bytes.size(), "", &error),
            "synthetic BIOS prefetch fixture");
    bus->set_bios(&firmware);
    gbarecomp::set_active_bus(bus.get());
    gbarecomp::set_active_ppu(ppu.get());
    runtime_init(bus.get());
    const DispatchEntry entries[] = {
        {kHandler, 0, 0, handler},
        {kWaitHandler, 0, 0, wait_handler},
    };
    const RuntimeNativeProgram program{entries, 2, nullptr, 0, nullptr, 0};
    runtime_select_native_program(&program);
    bus->write32(0x03007ffcu, kHandler);
    gba::bios_hle_set_interrupts_enabled(true);
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
    gba::bios_hle_set_interrupts_enabled(false);
    require(!g_bios_hle_dispatch_hook, "disabled HLE keeps original dispatch");
    gbarecomp::runtime_restore_arm_context(context);
    require(g_bios_hle_dispatch_hook == context.bios_dispatch_hook,
            "BIOS dispatch selection follows the bound CPU context");
    check_interrupt_wait(*bus);
    check_halt(*bus, true, true, false);
    check_halt(*bus, true, false, false);
    check_halt(*bus, false, true, true);
    gba::bios_hle_set_interrupts_enabled(false);
    runtime_select_native_program(nullptr);
    runtime_shutdown();
    gbarecomp::set_active_ppu(nullptr);
    gbarecomp::set_active_bus(nullptr);
    std::puts("Native IRQ callback, nesting, open bus and interrupt waits passed");
} catch (const std::exception& error) {
    std::fprintf(stderr, "Native IRQ check: %s\n", error.what());
    return 1;
}
