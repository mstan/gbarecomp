// gba_waitstates.h — GBA bus wait-state model: WAITCNT, the EWRAM control
// register, per-region access costs, opcode-fetch waits and the GamePak
// prefetch buffer.
//
// The table (RuntimeWaitTable, runtime_arm_types.h) is the single source of
// truth for every cycle the bus charges: data accesses (GbaBus::access_cycles,
// also DMA), opcode fetches (GbaBus::code_wait, mirrored inline by generated
// code through g_runtime_waits) and the prefetch stall. Values follow mGBA
// 1d201b22 src/gba/memory.c (GBA_BASE_WAITSTATES*, GBAAdjustWaitstates,
// GBAAdjustEWRAMWaitstates, GBAMemoryStall) and GBATEK "GBA System Control"
// (WAITCNT 4000204h, internal memory control 4000800h).

#pragma once

#include <cstdint>

#include "runtime_arm_types.h"

namespace gba {

// Power-on state: WAITCNT = 0 (ROM 4/2, SRAM 4, prefetch off) and EWRAM at
// its reset 2 wait states (memory control 0D000020h).
void waits_reset(RuntimeWaitTable& t);

// WAITCNT write (bits 0-10 wait states, bit 14 prefetch).
void waits_apply_waitcnt(RuntimeWaitTable& t, uint16_t waitcnt);

// Internal memory control write: EWRAM wait = 15 - bits 24-27. The value 15
// (0 wait states) locks up real hardware; mGBA rejects it and keeps the
// previous setting, and so do we. Returns false when rejected.
bool waits_apply_memctl(RuntimeWaitTable& t, uint32_t memctl);

// Cycles of one data access of `width` bytes at `addr` (base cycle + waits).
// 8-bit accesses cost the same as 16-bit ones; SRAM is an 8-bit bus whose
// every access costs its non-sequential 16-bit entry.
uint32_t waits_access_cycles(const RuntimeWaitTable& t, uint32_t addr,
                             uint8_t width, bool sequential);

// Wait states (beyond the base cycle) of one opcode fetch at `pc`.
uint32_t waits_code(const RuntimeWaitTable& t, uint32_t pc, bool thumb,
                    bool sequential);

// GamePak prefetch buffer: the adjusted stall of a `wait`-cycle data access
// or multiply by the instruction at `insn_pc`. Identity unless the code runs
// from the cartridge ROM with the prefetch buffer enabled; the caller only
// asks for accesses outside the cartridge space. Mutates the prefetch state
// (last_prefetched_pc). Exactly mGBA GBAMemoryStall.
int32_t waits_prefetch_stall(RuntimeWaitTable& t, int32_t wait,
                             uint32_t insn_pc, bool thumb);

}  // namespace gba
