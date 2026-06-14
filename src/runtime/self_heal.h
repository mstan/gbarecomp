// self_heal.h — Stage-1 self-healing dispatch-miss bookkeeping.
//
// When runtime_dispatch lands on a guest PC that has no generated function,
// runtime_dispatch_miss (src/runtime/runtime_arm_default_aborts.cpp) no longer
// aborts: it BRIDGES the missed call through the reference interpreter, then
// returns control to recompiled code (see that file for the stop-address
// contract). Every bridge is recorded here so the run can report — honestly —
// that it was NOT fully static, and so the miss list can seed a reviewed TOML
// proposal.
//
// Doctrine (PRINCIPLES.md "Honest self-healing" / "Coverage honesty is
// load-bearing"): a bridged miss MUST be (1) loudly logged, (2) healed toward
// static, and (3) fed back to the TOML via a *separate* proposal file that a
// human reviews and merges (never an auto-write to game.toml). Stage 1 does
// (1) + (3); the on-the-fly recompile of (2) is Stage 2.

#pragma once

#include <cstdint>

namespace gbarecomp {

// Reset all self-heal bookkeeping. Called once per machine bring-up
// (run_game) so a fresh process starts with a clean coverage tally.
void self_heal_reset();

// True if any dispatch miss was bridged this session.
bool self_heal_any_misses();

// Distinct missed PCs bridged this session.
std::uint32_t self_heal_distinct_misses();

// Total guest instructions executed in the interpreter bridge this session.
std::uint64_t self_heal_interpreted_insns();

// Emit the coverage-honesty banner to stdout and, if any misses occurred and
// frag_path is non-null, write the reviewed proposal fragment (a set of
// [[extra_func]] entries) to frag_path. NEVER merges into game.toml.
void self_heal_write_report(const char* frag_path);

}  // namespace gbarecomp
