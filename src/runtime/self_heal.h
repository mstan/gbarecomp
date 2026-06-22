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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
// frag_path is non-null, write the reviewed proposal fragment to frag_path:
// [[extra_func]] entries, plus a [[jump_table]]-candidate comment over any
// tight run of misses that looks like a computed-jump switch's case targets
// (see self_heal_cluster_misses). NEVER merges into game.toml.
void self_heal_write_report(const char* frag_path);

// Set the running program's identity (title, 4-char game code, ROM SHA-1).
// Stamped into the frag header and the coverage JSON so the persisted logs
// are unambiguous about which game produced them. Call once at startup.
void self_heal_set_program_identity(const char* title, const char* code,
                                    const char* sha1);

// ── Jump-table-aware proposal clustering ───────────────────────────
// One bridged miss, for the proposal/coverage report.
struct SelfHealMiss {
    std::uint32_t pc    = 0;
    bool          thumb = false;
    std::uint64_t count = 0;  // times bridged this session
};

// A maximal run of adjacent misses in a PC-sorted list. When
// `jump_table_candidate`, the run's members are tightly-spaced same-mode
// misses that look like the case targets of a computed-jump switch the
// finder could not size (e.g. Minish Cap Kid_Head's fragments in
// 0x08062894..0x08062920) — review should prefer ONE sized [[jump_table]]
// over merging the individual [[extra_func]] entries.
struct SelfHealCluster {
    std::size_t begin = 0;   // [begin, end) indices into the sorted input
    std::size_t end   = 0;
    bool jump_table_candidate = false;
};

// Group a PC-SORTED miss list into adjacency clusters. A run of at least
// `min_run` same-mode misses, each within `max_gap` bytes of its
// predecessor, is flagged jump_table_candidate. Every input index lands in
// exactly one returned cluster (singletons included). Pure / no I/O — unit
// tested (tests/selfheal/cluster_test.cpp) independently of the runtime.
std::vector<SelfHealCluster> self_heal_cluster_misses(
    const std::vector<SelfHealMiss>& sorted_misses,
    std::uint32_t max_gap, std::size_t min_run);

// JSON snapshot of the live miss/heal state for the TCP `misses` command and
// the exit coverage file: a top-level "coverage" verdict (FULLY_STATIC /
// NOT_STATIC), aggregate counters (distinct misses, interpreted insns,
// healed-to-native, native calls, in-flight, failed, jump-table-candidate
// regions) plus a per-bridged-PC array tagging healed state, native-call
// counts, and whether each PC is part of a jump-table-candidate run. A pure
// query of the always-on bookkeeping — no arming.
std::string self_heal_misses_json();

// Write self_heal_misses_json() to `path` (+ a trailing newline). Unlike the
// frag proposal, this is written on EVERY exit so the build loop / CI can
// read one machine-readable file to confirm FULLY_STATIC or drive the merge
// loop. Returns false if the path can't be opened. NEVER touches game.toml.
bool self_heal_write_coverage_json(const char* path);

}  // namespace gbarecomp
