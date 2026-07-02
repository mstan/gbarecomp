// cosim_state.h — full guest-architectural-state canonical hash for the GBA
// first-divergence co-simulation oracle. See COSIM_ORACLE.md.
//
// The single correctness rule (agnostic doc + PSX gold standard): this hash must
// cover EVERY piece of state that can influence future guest execution, and
// NOTHING host-only (function pointers, JIT/overlay tables, SDL buffers, malloc
// addresses, padding). A missed execution-relevant field is a blind spot (false
// "no divergence"); an included host-only field is a false positive. The
// validation gates (recomp-vs-recomp == 0, injected-fault halts at the right
// field) exist to prove this list is exactly right.
//
// R[15] (PC) is DELIBERATELY EXCLUDED from the cross-backend hash: the recomp
// keeps it current only at dispatch boundaries while the interpreter keeps it
// exact per-instruction, so at a mid-instruction cycle checkpoint they legitimately
// differ while in the same architectural state. A real control-flow split shows up
// as a differing GPR/memory value within one checkpoint (detection delayed by at
// most one checkpoint, never a blind spot). PC stays available via the `cpu` dump.
#pragma once

#include <cstdint>

namespace gbarecomp::cosim {

// Per-subsystem sub-hashes so a mismatch localizes to a subsystem before a full
// field dump. All 64-bit FNV-1a over canonical little-endian serialization.
struct SubHashes {
    uint64_t cpu = 0;      // R[0..14] (PC excluded), cpsr, banked sp/lr/spsr, r8-12 banks
    uint64_t iwram = 0;    // 32 KB @ 0x03000000
    uint64_t ewram = 0;    // 256 KB @ 0x02000000
    uint64_t vram = 0;     // 96 KB
    uint64_t pal = 0;      // 1 KB
    uint64_t oam = 0;      // 1 KB
    uint64_t io = 0;       // IRQ (IE/IF/IME) + timers (+prescaler phase) + DMA (+latched addrs) + WAITCNT
    uint64_t audio = 0;    // Direct-Sound FIFOs + 4 PSG channels + SOUNDCNT
    uint64_t ppu = 0;      // LCD I/O + window + affine reference-point latches + scanline/dot
    uint64_t save = 0;     // flash/SRAM engine state
    uint64_t prefetch = 0; // Game-Pak open-bus / BIOS prefetch latch
    uint64_t clock = 0;    // g_runtime_cycles (ARM7 master cycle)
};

// Compute the full canonical state hash of the LIVE machine (g_cpu + active
// bus/ppu). Fills *sub (may be null) and returns the folded top hash. Reads are
// side-effect-free (pure accessors / const serialize()).
uint64_t state_hash(SubHashes* sub);

// Reset any incremental / injection state (call once at machine init).
void state_reset();

// Gate-4 fault injection: after arming, the NEXT state_hash() perturbs one byte of
// IWRAM (region 0) / EWRAM (region 1) at `off`, or one CPU register (reg 0..14, or
// 16 = cpsr) by XOR, so the oracle MUST halt at exactly that field/subsystem.
void inject_ram(int region, uint32_t off, uint8_t xor_val);
void inject_reg(int reg_index, uint32_t xor_val);

}  // namespace gbarecomp::cosim
