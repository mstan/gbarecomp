// runtime_arm_default_aborts.cpp — production definitions of
// runtime_dispatch_miss + runtime_unimplemented_op.
//
// These would conceptually live alongside the rest of runtime_arm.cpp
// in gbarecomp_armv4t, but MinGW's PE-COFF target doesn't reliably
// resolve weak symbols out of static archives — so we can't use the
// "weak default + strong override" pattern that works on ELF. Instead
// we split: gbarecomp_armv4t has no defaults for these symbols, and
// every consumer must supply them.
//
//   * runtime_unimplemented_op — a CODEGEN gap (an IrOp the recompiler
//     hasn't lowered). This ALWAYS aborts loudly: it is a P0
//     codegen-completion task, never self-healed (PRINCIPLES.md
//     "Genuine interpreter gaps still abort loudly").
//
//   * runtime_dispatch_miss — a DISPATCH gap (a guest PC with no
//     generated function: the finder didn't reach it, or it was
//     deliberately excluded). As of Stage 1 this SELF-HEALS: it bridges
//     the missed call through the reference interpreter and returns
//     control to recompiled code, while loudly logging the miss and
//     recording it for a reviewed TOML proposal. See PRINCIPLES.md
//     "Honest self-healing" and src/runtime/self_heal.h.
//
// Test consumers (tests/codegen/stubs.cpp) link only gbarecomp_armv4t
// and supply their own versions that record state for the diff runner.

#include "runtime_arm.h"
#include "symbol_lookup.h"
#include "self_heal.h"
#include "overlay_loader.h"
#include "arm_cpu_bridge.h"
#include "runtime_bus_bridge.h"

#include "cpu_state.h"
#include "interpreter.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include "arm_ir.h"

#include "../gba/gba_bus.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace {

// ── Self-heal coverage bookkeeping ─────────────────────────────────
// Per-missed-PC record. `thumb` is the instruction-set state at the
// miss; `count` is how many times the bridge ran for this PC.
struct MissRec {
    bool          thumb = false;
    std::uint64_t count = 0;
};

std::map<std::uint32_t, MissRec> g_misses;        // keyed by PC & ~1
std::uint64_t                    g_interp_insns = 0;

// Hard backstop so a runaway bridge (e.g. an interpreted infinite loop
// that never reaches the stop address) fails loudly instead of hanging
// forever. Far above any real BIOS/cart subroutine length.
constexpr std::uint64_t kBridgeIterationCap = 200'000'000ull;

}  // namespace

namespace gbarecomp {

void self_heal_reset() {
    g_misses.clear();
    g_interp_insns = 0;
}

bool self_heal_any_misses() { return !g_misses.empty(); }

std::uint32_t self_heal_distinct_misses() {
    return static_cast<std::uint32_t>(g_misses.size());
}

std::uint64_t self_heal_interpreted_insns() { return g_interp_insns; }

void self_heal_write_report(const char* frag_path) {
    // Stage-2: a warm-cache run can heal PCs to native with ZERO misses this
    // session (the overlay tier catches them before the bridge). Healed-from-
    // cache is STILL NOT fully static (PRINCIPLES.md "Coverage honesty is
    // load-bearing"), so the banner must factor in heals, not just misses.
    std::uint64_t healed = 0, native_calls = 0, inflight = 0, failed = 0;
    gbarecomp::overlay_counters(&healed, &native_calls, &inflight, &failed);

    if (g_misses.empty() && healed == 0) {
        std::printf("self_heal_coverage=FULLY_STATIC dispatch_misses=0 "
                    "interpreted_insns=0 healed_native=0\n");
        return;
    }

    // Coverage honesty: this run was NOT fully static.
    std::printf("self_heal_coverage=NOT_STATIC dispatch_misses=%zu "
                "interpreted_insns=%llu healed_native=%llu native_calls=%llu "
                "inflight=%llu failed=%llu\n",
                g_misses.size(),
                static_cast<unsigned long long>(g_interp_insns),
                static_cast<unsigned long long>(healed),
                static_cast<unsigned long long>(native_calls),
                static_cast<unsigned long long>(inflight),
                static_cast<unsigned long long>(failed));
    // Build the PC-sorted miss list (std::map already orders by PC) and group
    // it once for both the banner summary and the frag: a tight run of
    // same-mode misses is the signature of a computed-jump switch's case
    // targets the finder couldn't size (the Kid_Head lesson,
    // 0x08062894..0x08062920). kJtMaxGap spans an un-hit case block between
    // two hit targets without merging genuinely separate functions.
    constexpr std::uint32_t kJtMaxGap = 0x80;  // bytes between case targets
    constexpr std::size_t   kJtMinRun = 3;     // run length to call it a table
    std::vector<gbarecomp::SelfHealMiss> sorted;
    sorted.reserve(g_misses.size());
    for (const auto& kv : g_misses)
        sorted.push_back({kv.first, kv.second.thumb, kv.second.count});
    const auto clusters =
        gbarecomp::self_heal_cluster_misses(sorted, kJtMaxGap, kJtMinRun);
    std::size_t jt_candidates = 0;
    for (const auto& c : clusters)
        if (c.jump_table_candidate) ++jt_candidates;

    if (!g_misses.empty()) {
        std::printf("  (the interpreter bridged %zu distinct PC(s) this session; "
                    "see %s for [[extra_func]] / [[jump_table]] proposals — "
                    "human-review + merge, never auto-merged)\n",
                    g_misses.size(), frag_path ? frag_path : "(none)");
        if (jt_candidates) {
            std::printf("  %zu jump-table candidate region(s) flagged — prefer a "
                        "sized [[jump_table]] over the per-PC [[extra_func]] "
                        "runs.\n", jt_candidates);
        }
    }
    for (const auto& kv : g_misses) {
        std::uint32_t off = 0;
        const char* sym = gba_symbol_lookup(kv.first, &off);
        std::uint64_t ncalls = 0;
        const bool is_healed =
            gbarecomp::overlay_query(kv.first, kv.second.thumb, &ncalls);
        std::printf("    bridged 0x%08X (%s) x%llu%s%s%s\n",
                    kv.first, kv.second.thumb ? "thumb" : "arm",
                    static_cast<unsigned long long>(kv.second.count),
                    is_healed ? " [HEALED->native]" : "",
                    sym ? " near " : "", sym ? sym : "");
        (void)ncalls;
    }

    if (g_misses.empty() || !frag_path) return;
    std::FILE* f = std::fopen(frag_path, "w");
    if (!f) {
        std::fprintf(stderr,
                     "self_heal: could not open %s for the miss proposal\n",
                     frag_path);
        return;
    }
    std::fprintf(f,
        "# recomp_master_misses.toml.frag — AUTO-GENERATED proposal.\n"
        "# These guest PCs were reached by runtime_dispatch with no generated\n"
        "# function and were bridged through the interpreter this session.\n"
        "# A HUMAN reviews these and merges the genuine ones into the binary's\n"
        "# config; this file is NEVER auto-merged (PRINCIPLES.md \"Never\n"
        "# auto-write game.toml\").\n"
        "#\n"
        "# Proposal shapes:\n"
        "#   [[extra_func]]  — a standalone missed function entry.\n"
        "#   [[jump_table]]  — flagged in a comment when a tight run of\n"
        "#       consecutive same-mode misses looks like a computed-jump\n"
        "#       switch's case targets. Sizing the table covers the whole\n"
        "#       switch in ONE entry; prefer it over the per-case [[extra_func]].\n"
        "# BIOS PCs (< 0x4000) belong in bios/gba_bios.toml; cart PCs in the\n"
        "# game's game.toml.\n\n");
    for (const auto& c : clusters) {
        if (c.jump_table_candidate) {
            std::fprintf(f,
                "# ── JUMP-TABLE CANDIDATE: %zu consecutive %s misses in "
                "[0x%08X, 0x%08X] ──\n"
                "# Likely the case targets of a computed-jump switch the finder\n"
                "# could not size. PREFER one sized [[jump_table]] over the %zu\n"
                "# [[extra_func]] below: find the abs32 table base (the\n"
                "# `ldr rT,[pc,#..]; add rT,index<<2; ldr/mov pc` dispatcher) and\n"
                "# add `[[jump_table]] addr=<base> stride=4 count=<CMP bound> "
                "format=\"abs32\" entries_mode=\"auto\"`.\n",
                c.end - c.begin, sorted[c.begin].thumb ? "thumb" : "arm",
                sorted[c.begin].pc, sorted[c.end - 1].pc, c.end - c.begin);
        }
        for (std::size_t k = c.begin; k < c.end; ++k) {
            std::fprintf(f,
                "[[extra_func]]\n"
                "addr = 0x%08X\n"
                "mode = \"%s\"\n"
                "note = \"proposed from self-heal miss-log; bridged x%llu%s\"\n\n",
                sorted[k].pc, sorted[k].thumb ? "thumb" : "arm",
                static_cast<unsigned long long>(sorted[k].count),
                c.jump_table_candidate
                  ? "; part of a JUMP-TABLE CANDIDATE (prefer a sized [[jump_table]])"
                  : "");
        }
    }
    std::fclose(f);
}

std::string self_heal_misses_json() {
    std::uint64_t healed = 0, native_calls = 0, inflight = 0, failed = 0;
    gbarecomp::overlay_counters(&healed, &native_calls, &inflight, &failed);

    // Reuse the proposal writer's jump-table-candidate grouping so the
    // machine-readable summary flags which misses are case targets of an
    // unsized computed-jump switch (build loop / CI can prefer a sized
    // [[jump_table]] over the per-PC [[extra_func]] runs).
    std::vector<gbarecomp::SelfHealMiss> sorted;
    sorted.reserve(g_misses.size());
    for (const auto& kv : g_misses)
        sorted.push_back({kv.first, kv.second.thumb, kv.second.count});
    const auto clusters = gbarecomp::self_heal_cluster_misses(sorted, 0x80, 3);
    std::map<std::uint32_t, bool> jt_pc;  // pc -> in a jump-table candidate run
    std::size_t jt_regions = 0;
    for (const auto& c : clusters) {
        if (c.jump_table_candidate) ++jt_regions;
        for (std::size_t k = c.begin; k < c.end; ++k)
            jt_pc[sorted[k].pc] = c.jump_table_candidate;
    }

    // FULLY_STATIC iff nothing was bridged AND nothing was healed (a warm
    // cache heals with zero misses but is still NOT fully static).
    const bool fully_static = g_misses.empty() && healed == 0;

    std::string out;
    char buf[384];
    std::snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"coverage\":\"%s\",\"enabled\":%s,"
        "\"distinct_misses\":%zu,\"interpreted_insns\":%llu,"
        "\"healed_native\":%llu,\"native_calls\":%llu,\"inflight\":%llu,"
        "\"failed\":%llu,\"jump_table_candidate_regions\":%zu,\"misses\":[",
        fully_static ? "FULLY_STATIC" : "NOT_STATIC",
        gbarecomp::overlay_was_enabled() ? "true" : "false",
        g_misses.size(),
        static_cast<unsigned long long>(g_interp_insns),
        static_cast<unsigned long long>(healed),
        static_cast<unsigned long long>(native_calls),
        static_cast<unsigned long long>(inflight),
        static_cast<unsigned long long>(failed),
        jt_regions);
    out += buf;

    bool first = true;
    for (const auto& kv : g_misses) {
        std::uint64_t ncalls = 0;
        const bool is_healed =
            gbarecomp::overlay_query(kv.first, kv.second.thumb, &ncalls);
        std::snprintf(buf, sizeof(buf),
            "%s{\"pc\":\"0x%08X\",\"mode\":\"%s\",\"bridged\":%llu,"
            "\"healed\":%s,\"native_calls\":%llu,\"jump_table_candidate\":%s}",
            first ? "" : ",", kv.first, kv.second.thumb ? "thumb" : "arm",
            static_cast<unsigned long long>(kv.second.count),
            is_healed ? "true" : "false",
            static_cast<unsigned long long>(ncalls),
            jt_pc[kv.first] ? "true" : "false");
        out += buf;
        first = false;
    }
    out += "]}";
    return out;
}

bool self_heal_write_coverage_json(const char* path) {
    if (!path) return false;
    std::FILE* f = std::fopen(path, "w");
    if (!f) {
        std::fprintf(stderr,
                     "self_heal: could not open %s for the coverage summary\n",
                     path);
        return false;
    }
    // Written on EVERY exit (unlike the frag, which only writes on misses) so
    // the build loop can read one file to confirm FULLY_STATIC or drive the
    // TOML merge loop. Same content as the TCP `misses` command.
    const std::string json = self_heal_misses_json();
    std::fwrite(json.data(), 1, json.size(), f);
    std::fputc('\n', f);
    std::fclose(f);
    return true;
}

}  // namespace gbarecomp

namespace {

// Record a bridged miss + loudly log the FIRST occurrence of each PC
// (subsequent occurrences only bump the counter, to avoid log spam on a
// hot bridged routine — the count is surfaced in the exit report).
void record_and_log_miss(std::uint32_t pc, bool thumb) {
    MissRec& r = g_misses[pc];
    bool first = (r.count == 0);
    r.thumb = thumb;
    ++r.count;
    if (first) {
        char symbuf[96];
        symbuf[0] = '\0';
        std::uint32_t off = 0;
        const char* sym = gba_symbol_lookup(pc, &off);
        if (sym) std::snprintf(symbuf, sizeof(symbuf), " <near %s+0x%X>", sym, off);
        std::fprintf(stderr,
            "runtime_arm: SELF-HEAL dispatch miss for pc=0x%08X%s (%s) — no "
            "generated function; bridging through the interpreter and "
            "recording for a TOML proposal (Stage-1 self-healing; NOT fully "
            "static). %s\n",
            pc, symbuf, thumb ? "thumb" : "arm",
            pc < 0x00004000u
                ? "Re-run `gba_recompile --bios bios/gba_bios.bin "
                  "--config bios/gba_bios.toml` after merging the proposal."
                : "Merge the proposal into game.toml and regenerate.");
    }
}

}  // namespace

// runtime_dispatch_miss — Stage-1 self-healing interpreter bridge.
//
// Stop-address contract (validated against the codegen direct-call ABI in
// src/armv4t/arm_codegen.cpp): the recompiled caller that reached this miss
// did `runtime_call_push_return(return_pc); runtime_dispatch(target)`. So the
// address control must resume at — when the missed subroutine returns — is the
// top of the call-return stack. We:
//   1. snapshot the call-return stack and capture stop_pc = top frame;
//   2. load g_cpu into the interpreter and point it at target_pc;
//   3. interpret the WHOLE missed call subtree (recompiled callees included)
//      purely, ticking each instruction so IRQs self-deliver via runtime_tick
//      -> runtime_irq into the recompiled BIOS, and routing SWIs via
//      runtime_swi, until the interpreter's PC reaches stop_pc;
//   4. write the interpreter state back into g_cpu and pop exactly the one
//      frame the recompiled callee's `bx lr` would have popped, so the caller
//      resumes as if the subroutine had been a normal recompiled call.
//
// Every interpreted instruction is fingerprinted into the SAME always-on ring
// the generated prologue uses (runtime_insn_fp), so a bridged run diffs
// bit-identically against a non-excluded build.
// runtime_bridge_interpret — the interpret-the-missed-subtree core, factored out
// of runtime_dispatch_miss so the P6 sljit differential gate can reuse it as its
// "interpreter pass": the kept, correct result a healed shard is validated
// against. Interprets from (entry_pc, entry_thumb) using the SAME stop-address
// contract and per-instruction tick (IRQ self-delivery + SWI routing) as the
// on-miss bridge, mutating g_cpu live. The miss log + heal enqueue belong to the
// dispatch-miss handler (below), not here.
extern "C" void runtime_bridge_interpret(uint32_t entry_pc, bool entry_thumb) {
    using gbarecomp::active_bus;

    gba::GbaBus* bus = active_bus();
    if (!bus) {
        // No bus bound (e.g. a unit harness that didn't set one up): we
        // cannot bridge. Stay loud — this is not a self-healable state.
        std::fprintf(stderr,
                     "runtime_arm: bridge interpret for pc=0x%08X with no active "
                     "bus — cannot bridge.\n", entry_pc);
        runtime_trace_dump_recent(96);
        std::abort();
    }

    // ── (1) Stop-address contract ──────────────────────────────────
    const std::uint32_t entry_depth = runtime_call_stack_depth();
    std::uint32_t stop_pc;
    bool top_level = (entry_depth == 0);
    if (!top_level) {
        stop_pc = runtime_call_stack_data()[entry_depth - 1] & ~1u;
    } else {
        // No pending direct-call return — a vector/top-level miss. This
        // should not happen for a BL-reached function; fall back to LR and
        // stay extra loud + capped.
        stop_pc = g_cpu.R[14] & ~1u;
        std::fprintf(stderr,
                     "runtime_arm: SELF-HEAL bridge entered at top level "
                     "(empty call-return stack) for pc=0x%08X; using LR=0x%08X "
                     "as the stop address.\n", entry_pc, stop_pc);
    }
    // Snapshot the entry stack so we can restore it exactly (minus the popped
    // frame) regardless of how IRQ/SWI excursions perturb it mid-bridge.
    std::vector<std::uint32_t> entry_stack;
    if (entry_depth > 0) {
        const std::uint32_t* data = runtime_call_stack_data();
        entry_stack.assign(data, data + entry_depth);
    }

    // ── (2) Enter the interpreter at target_pc ─────────────────────
    armv4t::CPUState cpu{};
    gbarecomp::load_arm_cpu_into_interp(g_cpu, cpu);
    cpu.R[15]    = entry_pc;
    cpu.cpsr.t   = entry_thumb;
    cpu.thumb    = entry_thumb;

    // ── (3) Interpret the subtree until control returns to stop_pc ──
    std::uint64_t iters = 0;
    std::uint64_t bridged_insns = 0;
    for (;;) {
        // Sync interp -> g_cpu (pre-execution state) so the fingerprint ring,
        // the BIOS-read gate, and any IRQ/SWI excursion all see exactly what
        // the generated per-instruction prologue would have set.
        gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
        const std::uint32_t pc = cpu.R[15];
        g_cpu.R[15] = pc;  // store_interp already set this; explicit for clarity
        bus->set_bios_access_enabled(pc < 0x00004000u);
        if (g_runtime_insn_trace) runtime_insn_fp();

        // Fetch + decode at the current PC.
        armv4t::Instr insn{};
        if (cpu.thumb) {
            insn = armv4t::ThumbDecoder::decode(bus->read16(pc), pc);
        } else {
            insn = armv4t::ArmDecoder::decode(bus->read32(pc), pc);
        }

        std::uint32_t cyc = 1;
        armv4t::Interpreter::Result r =
            armv4t::Interpreter::step(cpu, *bus, insn, &cyc);
        ++bridged_insns;

        if (r == armv4t::Interpreter::Result::NotImplemented ||
            r == armv4t::Interpreter::Result::Undefined) {
            // A genuine interpreter gap inside the bridged subtree. The
            // interpreter is the reference model; if IT can't execute this,
            // self-healing has nothing to fall back to. Stay loud (this is the
            // interpreter's own NotImplemented gate — PRINCIPLES.md "Genuine
            // interpreter gaps still abort loudly").
            gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
            std::fprintf(stderr,
                "runtime_arm: SELF-HEAL bridge hit interpreter %s at "
                "pc=0x%08X while bridging dispatch miss 0x%08X. The reference "
                "interpreter cannot execute this op; add its lowering in "
                "src/armv4t/interpreter.cpp.\n",
                r == armv4t::Interpreter::Result::Undefined ? "Undefined"
                                                            : "NotImplemented",
                pc, entry_pc);
            runtime_trace_dump_recent(96);
            std::abort();
        }

        if (r == armv4t::Interpreter::Result::Swi) {
            // Route the SWI through the recompiled BIOS exactly as generated
            // SWI codegen does: set the return address, then runtime_swi
            // (which masks IRQs, ticks the SWI's 3 cycles, and dispatches to
            // the recompiled BIOS SWI vector). Do NOT use the interpreter's
            // enter_swi, and do NOT additionally tick here.
            const std::uint32_t next_pc = pc + (cpu.thumb ? 2u : 4u);
            gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
            g_cpu.R[15] = next_pc;
            runtime_swi(insn.swi_imm);
            gbarecomp::load_arm_cpu_into_interp(g_cpu, cpu);
        } else {
            // Normal / Branched: tick the instruction's cost. runtime_tick
            // self-delivers any pending IRQ via runtime_irq into the
            // recompiled BIOS (mutating g_cpu), so sync around it.
            gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
            runtime_tick(cyc);
            gbarecomp::load_arm_cpu_into_interp(g_cpu, cpu);
        }

        if ((cpu.R[15] & ~1u) == stop_pc) break;

        if (++iters > kBridgeIterationCap) {
            gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
            std::fprintf(stderr,
                "runtime_arm: SELF-HEAL bridge for 0x%08X exceeded %llu "
                "instructions without returning to stop_pc=0x%08X (current "
                "pc=0x%08X). Aborting rather than spinning silently.\n",
                entry_pc,
                static_cast<unsigned long long>(kBridgeIterationCap),
                stop_pc, cpu.R[15]);
            runtime_trace_dump_recent(96);
            std::abort();
        }
    }

    g_interp_insns += bridged_insns;

    // ── (4) Write back + pop the returned frame ────────────────────
    gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);  // g_cpu.R[15] == stop_pc
    bus->set_bios_access_enabled(g_cpu.R[15] < 0x00004000u);
    if (!top_level) {
        // Restore the entry stack minus exactly the one frame the missed
        // subroutine's return idiom (`bx lr` / `pop {pc}`) would have popped
        // via runtime_call_should_return.
        runtime_call_stack_restore(entry_stack.data(), entry_depth - 1);
    }
    // Control returns to the recompiled caller's BL site, which checks
    // g_cpu.R[15] == its pushed return address (== stop_pc) and continues.
}

// runtime_dispatch_miss — Stage-1 self-healing entry: log the miss, enqueue a
// Stage-2 heal (so the NEXT hit dispatches native), then bridge through the
// interpreter via runtime_bridge_interpret. See that function's note above.
extern "C" void runtime_dispatch_miss(uint32_t target_pc) {
    const std::uint32_t entry_pc = target_pc & ~1u;
    const bool entry_thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0;
    record_and_log_miss(entry_pc, entry_thumb);
    gbarecomp::overlay_request_compile(entry_pc, entry_thumb);
    runtime_bridge_interpret(entry_pc, entry_thumb);
}

extern "C" void runtime_unimplemented_op(const char* op_name,
                                          uint32_t pc) {
    std::fprintf(stderr,
                 "runtime_arm: unimplemented op %s at pc=0x%08X "
                 "(a CODEGEN gap, not a dispatch miss — NOT self-healed). "
                 "Add the lowering in src/armv4t/arm_codegen.cpp.\n",
                 op_name ? op_name : "(null)", pc);
    runtime_trace_dump_recent(96);
    std::abort();
}
