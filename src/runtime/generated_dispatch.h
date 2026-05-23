// generated_dispatch.h — STUB. Address → function dispatch.
//
// The recompiler emits a dispatch table keyed by guest PC, with one
// entry per discovered function (split by ARM/THUMB mode). Indirect
// calls (BX, LDM with PC, BL into runtime-known address) route here.
//
// On dispatch miss the runtime:
//   1. Records the (addr, mode) to a ring.
//   2. Writes the line to `dispatch_misses.log` next to the executable.
//   3. Returns control with an error so the game (or the smoke test)
//      can observe it.

#pragma once

#include <cstdint>

namespace gbarecomp {

// Called by generated code when it doesn't have a function for `addr`.
void on_dispatch_miss(uint32_t addr, bool thumb);

}  // namespace gbarecomp
