#pragma once

#include "runtime_arm.h"
#include <array>
#include <cstdint>
#include <vector>

namespace gbarecomp {
// Host exception metadata that used to exist only in runtime_irq's C++ frame.
// Kept separate from the guest's banked SPSRs/registers in ArmCpuState.
struct RuntimeIrqContinuation {
    std::array<std::uint32_t, 5> preserved{}; // r0-r3, r12
    std::uint32_t return_floor = 0;
    std::uint32_t return_depth = 0;
    std::uint32_t iret_depth = 0;
};

// Mutable execution state for one guest. The legacy C ABI remains a bound
// execution window, so generated functions can be shared without rewriting
// register accesses. Binding/capturing is legal only outside native dispatch.
struct RuntimeArmContext {
    ArmCpuState cpu{};
    std::vector<std::uint32_t> returns;
    std::vector<RuntimeIrqContinuation> interrupts;
    std::uint32_t return_floor = 0;
    std::uint32_t irq_depth = 0;
    std::uint32_t iret_depth = 0;
    std::uint32_t resume_pc = 0;
    std::uint32_t irq_from_halt = 0;
    std::uint64_t irq_entries = 0;
    RuntimeThumbAluImmediateOverride immediate_override = nullptr;
    RuntimeBusReadOverride read_override = nullptr;
    RuntimeRamDispatchHook ram_dispatch = nullptr;
    RuntimeForceInterpHook force_interp = nullptr;
    void (*entry_hook)(std::uint32_t) = nullptr;
    int (*bios_hook)(std::uint32_t) = nullptr;
    // Optional per-program dispatch, required when heterogeneous images are
    // implemented. nullptr uses this executable's shared generated tables.
    int (*program_dispatch)(std::uint32_t, int) = nullptr;
};

void runtime_capture_arm_context(RuntimeArmContext&);
void runtime_restore_arm_context(const RuntimeArmContext&);
// Enables exception continuations for the duration of a dispatch, independently
// of the legacy single-machine runner. The owner catches RuntimeDispatchYield
// only after normal C++ unwinding, then captures the context.
void runtime_set_resumable_context(RuntimeArmContext*);
bool runtime_has_resumable_context();
struct RuntimeDispatchYield {};
} // namespace gbarecomp
