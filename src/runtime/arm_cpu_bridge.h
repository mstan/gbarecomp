// arm_cpu_bridge.h — convert between the two ARM7TDMI register-file
// representations this codebase carries:
//
//   * ArmCpuState / g_cpu  (runtime_arm.h)  — the recompiler ABI: a flat
//     C struct generated code reads/writes, CPSR packed into one u32.
//   * armv4t::CPUState     (cpu_state.h)     — the interpreter's view,
//     CPSR as a bitfield, otherwise the same fields.
//
// Both keep the ACTIVE register window in R[0..15] and the inactive
// copies in banked_sp/lr/spsr + r8_12_{user,fiq}; the bank-index order
// is identical (User=0 .. Undefined=5 == armv4t::Bank_Count). So the
// conversion is a field-by-field copy plus a CPSR pack/unpack — there is
// no mode-swap step, because the source already has the correct window
// live in R[] (the recompiler maintains this via runtime_msr_cpsr, the
// interpreter via its own mode handling).
//
// Used by:
//   * the runtime self-healing interpreter fallback (Stage 1), to enter
//     the interpreter from recompiled state at a dispatch miss and write
//     the result back when control returns to recompiled code;
//   * bios_smoke's savestate-into-interpreter oracle path.

#pragma once

#include "cpu_state.h"     // armv4t::CPUState
#include "runtime_arm.h"   // ArmCpuState

namespace gbarecomp {

// Recompiler ABI register file -> interpreter register file.
void load_arm_cpu_into_interp(const ArmCpuState& src, armv4t::CPUState& dst);

// Interpreter register file -> recompiler ABI register file.
void store_interp_into_arm_cpu(const armv4t::CPUState& src, ArmCpuState& dst);

}  // namespace gbarecomp
