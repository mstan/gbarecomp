// runtime_arm_default_aborts.cpp — production abort stubs for
// runtime_dispatch_miss + runtime_unimplemented_op.
//
// These would conceptually live alongside the rest of runtime_arm.cpp
// in gbarecomp_armv4t, but MinGW's PE-COFF target doesn't reliably
// resolve weak symbols out of static archives — so we can't use the
// "weak default + strong override" pattern that works on ELF. Instead
// we split: gbarecomp_armv4t has no defaults for these symbols, and
// every consumer must supply them.
//
// Production consumers (anything that links gbarecomp_runtime) get
// the strong abort versions defined here — same semantics as the
// previous in-tree weak defaults: log the gap and abort. The aborts
// are the Phase 2.8 gate (PRINCIPLES.md "Interpreter is informative,
// never load-bearing (SHOWSTOPPER)").
//
// Test consumers (tests/codegen/stubs.cpp) link only gbarecomp_armv4t
// and supply their own non-aborting versions that record state for
// the diff runner.

#include "runtime_arm.h"
#include "symbol_lookup.h"

#include <cstdio>
#include <cstdlib>

extern "C" void runtime_dispatch_miss(uint32_t target_pc) {
    char symbuf[96];
    symbuf[0] = '\0';
    uint32_t off = 0;
    const char* sym = gba_symbol_lookup(target_pc, &off);
    if (sym) std::snprintf(symbuf, sizeof(symbuf), " <near %s+0x%X>", sym, off);
    std::fprintf(stderr,
                 "runtime_arm: dispatch miss for pc=0x%08X%s "
                 "(no generated function; not recompiled, "
                 "or function-finder didn't reach it). "
                 "If this is a cart address, add to game.toml "
                 "[functions] and regenerate. If this is a BIOS "
                 "address (< 0x4000), run `gba_recompile --bios "
                 "bios/gba_bios.bin` and rebuild.\n",
                 target_pc, symbuf);
    runtime_trace_dump_recent(96);
    std::abort();
}

extern "C" void runtime_unimplemented_op(const char* op_name,
                                          uint32_t pc) {
    std::fprintf(stderr,
                 "runtime_arm: unimplemented op %s at pc=0x%08X "
                 "(per PRINCIPLES.md \"Interpreter is informative, "
                 "never load-bearing\"). Add the lowering in "
                 "src/armv4t/arm_codegen.cpp.\n",
                 op_name ? op_name : "(null)", pc);
    runtime_trace_dump_recent(96);
    std::abort();
}
