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
    // Optional pure PC/mode predicate: no bus reads or guest mutations. False
    // guarantees the RAM hook would decline, allowing normal generated RAM
    // code to retain native batching. nullptr conservatively visits the hook.
    int (*ram_dispatch_filter)(std::uint32_t, int) = nullptr;
    // Trusted scheduler wiring, not guest state. Runs before a RAM callback can
    // inspect mutable memory, including calls nested inside generated code.
    void (*ram_dispatch_boundary)(std::uint32_t) = nullptr;
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
// Generated calls already propagate an interrupted PC back to the dispatcher.
// Ordinary scheduler yields may use that return path, preserving guest return
// frames instead of cancelling them, including explicit IRQ continuations.
// Enable only around generated dispatch; callbacks keep the exception path.
// Not snapshot state.
// Setting either value clears the previous dispatch's pending suspension.
void runtime_set_return_yield(bool enabled);
bool runtime_suspend_dispatch(); // returns true, or throws RuntimeDispatchYield
bool runtime_dispatch_suspended(); // also restores the suspended guest PC
} // namespace gbarecomp
