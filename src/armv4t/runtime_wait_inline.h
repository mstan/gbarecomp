// runtime_wait_inline.h — opcode-fetch / prefetch timing helpers inlined
// into generated code. Included by runtime_arm.h (static build) and by the
// Stage-2 overlay shim; each includer first makes `g_runtime_waits` (the
// active bus's `RuntimeWaitTable`) and `runtime_prefetch_stall_delta`
// visible, so the generated bodies stay byte-identical across both builds.
//
// The table belongs to the active bus (gba::GbaBus::wait_table) and follows
// WAITCNT and the EWRAM control register. Codegen constant-folds the fetch
// waits of fixed-bus code regions and reads this table only for EWRAM and
// cartridge code; these helpers cover what is only known at run time. The
// arithmetic is runtime_wait_model.h, the same code the bus model runs.

#pragma once

#include <stdint.h>

#include "runtime_wait_model.h"

// Pipeline refill after a PC write: the N+S opcode fetch pair in the target
// region and instruction set (mGBA ARMWritePC/ThumbWritePC wait part; the
// two base cycles are already in the instruction's fixed cost).
static inline uint32_t runtime_refill_cycles(uint32_t target_pc,
                                             uint32_t thumb) {
    return rwt_code_wait(&g_runtime_waits, target_pc, thumb, 0u) +
           rwt_code_wait(&g_runtime_waits, target_pc, thumb, 1u);
}

// GamePak prefetch-buffer adjustment of a `wait`-cycle stall by the
// cartridge instruction at `pc` (emitted only for cartridge code). Data
// accesses to the cartridge space itself stop the prefetcher, so they are
// never adjusted; a multiply passes addr 0. Returns the (wrapping) delta to
// add. The buffer model itself stays out of line (one call only while the
// buffer is enabled) to keep generated code small.
static inline uint32_t runtime_prefetch_adjust(uint32_t wait, uint32_t addr,
                                               uint32_t pc, uint32_t thumb) {
    if (!g_runtime_waits.prefetch || addr >= 0x08000000u) return 0u;
    return runtime_prefetch_stall_delta(wait, pc, thumb);
}
