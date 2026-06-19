// overlay_sljit_heal.h — the in-process sljit producer for the self-heal tier.
//
// The sljit counterpart to overlay_emit's gcc path: instead of emit C → g++ →
// DLL → LoadLibrary (async, needs a toolchain), it discovers the function at a
// dispatch-miss PC, decodes its extent, and JITs it in-process via
// emit_function_sljit (sync, sub-ms, NO toolchain). The produced function is a
// `void fn(void)` operating on the host g_cpu + runtime ABI (the emitter bakes
// those addresses directly) — drop-in for HealedEntry.fn, dispatched by
// overlay_try_dispatch exactly like a gcc DLL func.
//
// Precision over recall: declines (returns false) if the function can't be
// discovered or contains ANY instruction the emitter doesn't yet lower, so the
// caller keeps the honest interpreter bridge (and a toolchain machine may still
// fall to gcc).

#pragma once

#include <cstddef>
#include <cstdint>

namespace gbarecomp {

// Produce a native shard for the function rooted at (pc, thumb) in the immutable
// code image [bytes, bytes+size) based at `base`. On success: *out_fn = the host
// entry, *out_code = the sljit code block (kept for the process lifetime; the OS
// reclaims it at exit), *out_end = the function's exclusive end address,
// *out_leaf = true iff the function is GATE-ELIGIBLE for the P6 differential gate
// — its control flow is guaranteed to C-return cleanly to the gate: no computed/
// indirect transfer-that-isn't-a-return and no direct tail branch escaping the
// extent. Direct calls (BL/BLX-imm) ARE eligible (their host-stack recursion is
// handled by the gate's depth bound). Returns true. Returns false on decline.
// out_leaf may be null. (Named `leaf` for ABI continuity; it no longer means
// "no calls".)
bool overlay_sljit_produce(uint32_t pc, bool thumb,
                           const uint8_t* bytes, std::size_t size, uint32_t base,
                           void (**out_fn)(void), void** out_code,
                           uint32_t* out_end, bool* out_leaf = nullptr);

// True iff the function rooted at (pc, thumb) is discoverable AND every
// instruction in its extent is lowerable by the emitter (sljit_supports). Used
// by the force-heal demo to pick supported functions without producing code.
bool overlay_sljit_function_supported(uint32_t pc, bool thumb,
                                      const uint8_t* bytes, std::size_t size,
                                      uint32_t base);

}  // namespace gbarecomp
