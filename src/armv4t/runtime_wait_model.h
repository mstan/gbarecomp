// runtime_wait_model.h — pure opcode-fetch / GamePak-prefetch timing over a
// RuntimeWaitTable (runtime_arm_types.h). No globals: shared verbatim by the
// bus model (src/gba/gba_waitstates.cpp, i.e. the interpreter's
// Bus::code_wait / prefetch_stall) and the helpers inlined into generated
// code (runtime_wait_inline.h), so both execution paths run one
// implementation.

#pragma once

#include <stdint.h>

#include "runtime_arm_types.h"

// Wait states (beyond the base cycle) of one opcode fetch at `pc`: 16-bit for
// THUMB, 32-bit for ARM, sequential or not. Regions above 0Fh never hold
// code and cost nothing extra.
static inline uint32_t rwt_code_wait(const RuntimeWaitTable* t, uint32_t pc,
                                     uint32_t thumb, uint32_t sequential) {
    const uint32_t region = pc >> 24;
    if (region > 0xFu) return 0u;
    if (thumb) return sequential ? t->s16[region] : t->n16[region];
    return sequential ? t->s32[region] : t->n32[region];
}

// GamePak prefetch buffer: the adjusted stall of a `wait`-cycle data access
// or multiply by the instruction at `insn_pc` (exactly mGBA 1d201b22
// src/gba/memory.c GBAMemoryStall). Identity unless the code runs from the
// cartridge ROM (regions 08h-0Dh) with WAITCNT bit 14 set; callers only ask
// for data accesses outside the cartridge space. Updates
// t->last_prefetched_pc. The result may be negative: opcodes the buffer
// fetched during the stall make the following fetches free.
static inline int32_t rwt_prefetch_stall(RuntimeWaitTable* t, int32_t wait,
                                         uint32_t insn_pc, uint32_t thumb) {
    const uint32_t region = insn_pc >> 24;
    if (region < 0x8u || region > 0xDu || !t->prefetch)
        return wait;  // the wait is the stall

    // mGBA reads gprs[ARM_PC], which runs two instructions ahead.
    const uint32_t pc = insn_pc + (thumb ? 4u : 8u);

    // Don't prefetch too much if this overlaps a previous prefetch.
    int32_t previous_loads = 0;
    int32_t max_loads = 8;
    const uint32_t dist = t->last_prefetched_pc - pc;
    if (dist < 16u) {
        previous_loads = (int32_t)(dist >> 1);
        max_loads -= previous_loads;
    }

    // Sequential halfword fetches the buffer completes during the stall.
    const int32_t s = t->s16[region];
    int32_t stall = s + 1;
    int32_t loads = 1;
    while (stall < wait && loads < max_loads) {
        stall += s;
        ++loads;
    }
    t->last_prefetched_pc = pc + 2u * (uint32_t)(loads + previous_loads - 1);

    // The wait cannot take less time than the prefetch stalls.
    if (stall > wait) wait = stall;
    // The next opcode fetch was charged as N; it becomes an S.
    wait -= (int32_t)t->n16[region] - s;
    // The next `loads` S fetches are already done.
    wait -= stall;
    return wait;
}
