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
constexpr uint32_t kSvcMode = 0x13u;
constexpr uint32_t kSystemMode = 0x1fu;
constexpr uint32_t kSwiVector = 0x08u;
constexpr uint32_t kSwiReturn = 0x188u;
constexpr uint32_t kHaltResume = 0x1b4u;
constexpr uint32_t kWaitResume = 0x348u;
constexpr uint32_t kWaitFlags = 0x03fffff8u;
constexpr uint32_t kIoBase = 0x04000000u;
constexpr uint32_t kMasterEnable = kIoBase + 0x208u;
constexpr uint32_t kHaltControl = kIoBase + 0x301u;
constexpr uint32_t kIrqMask = 0x80u;
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

template<std::size_t N>
uint32_t push_frame(const std::array<uint32_t, N>& data) {
    const uint32_t stack = g_cpu.R[13] - 4u * N;
    uint32_t cycles = 1;
    for (std::size_t i = 0; i < N; ++i) {
        const uint32_t address = (stack + 4u * i) & ~3u;
        cycles += runtime_mem_cycles(address, 4, i != 0);
        bus_write_u32(address, data[i]);
    }
    g_cpu.R[13] = stack;
    return cycles;
}

template<std::size_t N>
uint32_t pop_frame(std::array<uint32_t, N>& data) {
    const uint32_t stack = g_cpu.R[13];
    uint32_t cycles = 2;
    for (std::size_t i = 0; i < N; ++i) {
        const uint32_t address = (stack + 4u * i) & ~3u;
        cycles += runtime_mem_cycles(address, 4, i != 0);
        data[i] = bus_read_u32(address);
    }
    g_cpu.R[13] = stack + 4u * N;
    return cycles;
}

uint32_t consume_interrupt_flags() {
    // The cartridge's handler populates the software latch. Only consume its
    // selected bits. IME is enabled on every exit, including a spurious wake.
    bus_write_u8(kMasterEnable, 0);
    const uint16_t flags = bus_read_u16(kWaitFlags);
    const uint16_t accepted = flags & g_cpu.R[1];
    g_cpu.R[0] = accepted;
    g_cpu.R[2] = flags & ~accepted;
    if (accepted) bus_write_u16(kWaitFlags, g_cpu.R[2]);
    bus_write_u8(kMasterEnable, 1);
    return accepted ? 15u : 14u;
}

void wait_at_interrupt() {
    // Existing HALT scheduling pumps PPU, audio and timers and performs IRQ
    // delivery with the existing wake latency. A saved PC and the guest's two
    // stack frames carry the suspended service across host steps and snapshots.
    g_cpu.R[15] = kWaitResume;
    bus_write_u8(kHaltControl, 0);
    runtime_tick(2);
}

void return_from_swi(GbaBus& bus, uint32_t cycles) {
    std::array<uint32_t, 2> caller_frame;
    cycles += pop_frame(caller_frame) + 2;
    g_cpu.R[2] = caller_frame[0];
    g_cpu.R[14] = caller_frame[1];
    runtime_msr_cpsr(kSvcMode | 0xc0u, 9);
    std::array<uint32_t, 1> status;
    std::array<uint32_t, 3> supervisor;
    cycles += pop_frame(status) + 1;
    runtime_msr_spsr(status[0], 9);
    cycles += pop_frame(supervisor) + 3;
    g_cpu.R[11] = supervisor[0];
    g_cpu.R[12] = supervisor[1];
    g_cpu.R[14] = supervisor[2];
    g_cpu.R[15] = kSwiReturn;
    bus.latch_bios_prefetch(kSwiReturn, false);
    const uint32_t next = g_cpu.R[14];
    runtime_tick(cycles + runtime_exception_return_refill(next));
    runtime_exception_return(next);
}

void wait_return(GbaBus& bus) {
    std::array<uint32_t, 2> wait_frame;
    const uint32_t cycles = pop_frame(wait_frame) + 3;
    g_cpu.R[4] = wait_frame[0];
    return_from_swi(bus, cycles);
}

bool enter_interrupt_service(GbaBus& bus) {
    const uint32_t caller_status = g_cpu.banked_spsr[ARM_BANK_SUPERVISOR];
    // ARM puts the function in immediate bits 23:16 and Thumb in bits 7:0.
    // In either instruction set the byte two positions before LR holds it.
    const uint32_t swi_address = g_cpu.R[14] - 2u;
    const uint32_t swi = bus.read8(swi_address);
    if (swi != 2u && swi != 4u && swi != 5u) return false;

    // Preserve the supervisor return state and run the service in System mode
    // with the caller's IRQ mask. The cartridge IRQ handler sees its ordinary
    // banks and stack rather than a live host frame across the wait.
    uint32_t cycles = 12 + runtime_mem_cycles(swi_address, 1, 0);
    cycles += push_frame(std::array{g_cpu.R[11], g_cpu.R[12], g_cpu.R[14]});
    cycles += push_frame(std::array{caller_status});
    g_cpu.R[11] = kSystemMode | (caller_status & kIrqMask);
    runtime_tick(cycles);
    runtime_msr_cpsr(g_cpu.R[11], 9);
    cycles = push_frame(std::array{g_cpu.R[2], g_cpu.R[14]}) + 4;
    if (swi == 2u) {
        // Standalone Halt preserves all caller registers and never consumes
        // the software wait latch. Its parked PC and guest frames survive a
        // save made while the devices are still running.
        g_cpu.R[14] = 0x170u;
        g_cpu.R[15] = kHaltResume - 4u;
        g_cpu.R[2] = 0;
        g_cpu.R[12] = kIoBase;
        runtime_tick(cycles + 5);
        g_cpu.R[15] = kHaltResume;
        bus.latch_bios_prefetch(kHaltResume, false);
        bus_write_u8(kHaltControl, 0);
        runtime_tick(2);
        return true;
    }
    cycles += push_frame(std::array{g_cpu.R[4], 0x170u});
    g_cpu.R[3] = 0;
    g_cpu.R[4] = 1;
    g_cpu.R[12] = kIoBase;
    if (swi == 5u) {
        g_cpu.R[0] = g_cpu.R[1] = 1;
        cycles += 2;
    }
    const bool discard_previous = g_cpu.R[0] != 0;
    g_cpu.R[14] = discard_previous ? kWaitResume - 4u : kWaitResume + 4u;
    g_cpu.R[15] = kWaitResume;
    cycles += 3 + (discard_previous ? 3u : 1u) + consume_interrupt_flags();
    runtime_tick(cycles);
    if (!discard_previous && g_cpu.R[0]) wait_return(bus);
    else wait_at_interrupt();
    return true;
}

int dispatch_firmware(uint32_t pc, int thumb) {
    if (thumb || !gbarecomp::native_service_window_allowed()) return 0;
    const uint32_t mode = g_cpu.cpsr & 0x1fu;
    auto* bus = gbarecomp::active_bus();
    if (!bus) return 0;
    if (pc == kSwiVector && mode == kSvcMode) return enter_interrupt_service(*bus);
    if (pc == kHaltResume && mode == kSystemMode && g_cpu.R[14] == 0x170u &&
        g_cpu.R[12] == kIoBase) {
        return_from_swi(*bus, 3);
        return 1;
    }
    if (pc == kWaitResume && mode == kSystemMode && g_cpu.R[12] == kIoBase &&
        g_cpu.R[3] == 0 && g_cpu.R[4] == 1) {
        g_cpu.R[14] = kWaitResume + 4u;
        const uint32_t check_cost = consume_interrupt_flags();
        runtime_tick(3u + check_cost + (g_cpu.R[0] ? 1u : 3u));
        if (g_cpu.R[0]) wait_return(*bus);
        else wait_at_interrupt();
        return 1;
    }
    if ((pc != kIrqVector && pc != kIrqReturn) || !g_irq_nest_depth ||
        mode != kIrqMode || !(g_cpu.cpsr & kIrqMask))
        return 0;
    g_runtime_resume_pc = 0;
    g_cpu.R[15] = pc;
    if (pc == kIrqVector) irq_enter(*bus);
    else irq_return(*bus);
    return 1;
}

}  // namespace

void bios_hle_set_interrupts_enabled(bool enabled) {
    g_bios_hle_dispatch_hook = enabled ? &dispatch_firmware : nullptr;
}

}  // namespace gba
