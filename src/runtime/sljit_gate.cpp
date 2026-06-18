// sljit_gate.cpp — see sljit_gate.h.

#include "sljit_gate.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "heal_gate.h"
#include "runtime_arm.h"          // g_cpu, g_runtime_cycles, shadow-tick, bridge,
                                  // call-return stack accessors
#include "runtime_bus_bridge.h"   // gbarecomp::active_bus

// IRQ-entry counter (incremented by runtime_irq on every delivery). A nonzero
// delta across the interpreter pass means an IRQ vectored mid-function, which a
// shadow-ticked shard re-run can't reproduce → pin.
extern "C" unsigned long long g_runtime_irq_entries;

namespace gbarecomp::sljit_gate {

namespace {

struct GateState {
    enum class Status { Validating, Promoted, Pinned };
    Status   status = Status::Validating;
    unsigned clean_passes = 0;
};

// Game-thread-only (same as g_healed) → no locking.
std::unordered_map<uint64_t, GateState> g_state;

// Re-entrancy guard: while a shard's shadow validation run is in flight, any
// nested dispatch (a transfer the static leaf check didn't catch) runs blind
// rather than recursively validating. The whole shadow run is rolled back, so
// nested blind execution is harmless.
bool g_gate_active = false;

int      g_enabled   = -1;   // -1 = unresolved, 0/1 = resolved
unsigned g_threshold = 3;    // consecutive clean passes required to promote

uint64_t key(uint32_t pc, bool thumb) {
    return (static_cast<uint64_t>(pc & ~1u) << 1) | (thumb ? 1u : 0u);
}

void resolve_enabled() {
    const char* e = std::getenv("GBARECOMP_HEAL_SLJIT_LIVE");
    if (!e || !*e || e[0] == '0' || e[0] == 'n' || e[0] == 'N' ||
        e[0] == 'f' || e[0] == 'F') {
        g_enabled = 0;
        return;
    }
    g_enabled = 1;
    // A numeric value > 1 overrides the consecutive-clean threshold.
    const long n = std::strtol(e, nullptr, 10);
    if (n > 1) g_threshold = static_cast<unsigned>(n);
    std::printf("sljit_gate: differential gate ENABLED "
                "(promote after %u consecutive clean passes)\n", g_threshold);
}

// Run one validation pass: interpret the function (kept result), then re-run the
// shard in shadow from the same snapshot and diff. Updates `st` (promote/pin).
// On return, the live machine state IS the interpreter result either way.
void validate(uint32_t pc, bool thumb, void (*fn)(void), GateState& st) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) { st.status = GateState::Status::Pinned; return; }

    const uint32_t depth0 = runtime_call_stack_depth();
    if (depth0 == 0) {
        // Top-level (no pending return) — the rare vector case. Don't shadow-
        // validate it; just interpret (kept result) and pin.
        runtime_bridge_interpret(pc, thumb);
        st.status = GateState::Status::Pinned;
        std::printf("sljit_gate: PIN 0x%08X (%s) — top-level dispatch, not "
                    "shadow-validatable\n", pc, thumb ? "thumb" : "arm");
        return;
    }

    g_gate_active = true;

    // ── Snapshot the pre-call state S0 (cpu + cycles + RAM + call stack) ──
    const std::vector<uint32_t> stack0(runtime_call_stack_data(),
                                       runtime_call_stack_data() + depth0);
    heal_gate::StateSnapshot s0 =
        heal_gate::capture_full(g_cpu, g_runtime_cycles, *bus);
    const uint64_t irq0 = g_runtime_irq_entries;

    // ── Interpreter pass — the kept, correct result (live) ──
    heal_gate::Journal io_probe;
    io_probe.arm(*bus, heal_gate::Journal::Mode::Record);  // detect IO touch
    runtime_bridge_interpret(pc, thumb);
    io_probe.disarm(*bus);
    const bool io  = io_probe.io_touched();
    const bool irq = (g_runtime_irq_entries != irq0);

    heal_gate::StateSnapshot s_interp =
        heal_gate::capture_full(g_cpu, g_runtime_cycles, *bus);
    const uint32_t depth_interp = runtime_call_stack_depth();
    const std::vector<uint32_t> stack_interp(
        runtime_call_stack_data(), runtime_call_stack_data() + depth_interp);

    if (io || irq) {
        // Side-effectful (IO) or IRQ-crossing — can't be cleanly shadow-replayed.
        // Pin; the live state already IS the interpreter result.
        st.status = GateState::Status::Pinned;
        std::printf("sljit_gate: PIN 0x%08X (%s) — %s during the interpreter "
                    "pass (not shadow-validatable)\n", pc, thumb ? "thumb" : "arm",
                    io ? "touched IO" : "crossed an IRQ");
        g_gate_active = false;
        return;
    }

    // ── Restore to S0 and re-run the SHARD in shadow mode ──
    runtime_call_stack_restore(stack0.data(), depth0);
    heal_gate::restore_full(s0, g_cpu, g_runtime_cycles, *bus);

    heal_gate::Journal trap;
    trap.arm(*bus, heal_gate::Journal::Mode::Shadow);  // trap any device write
    g_runtime_shadow_tick = 1;
    g_runtime_shadow_cycles = 0;
    fn();
    g_runtime_shadow_tick = 0;
    trap.disarm(*bus);
    const bool shard_io = trap.io_touched();

    heal_gate::StateSnapshot s_shard =
        heal_gate::capture_full(g_cpu, g_runtime_cycles, *bus);
    // Shadow ticks didn't advance g_runtime_cycles; the shard's real cost is the
    // shadow accumulator. Compare like-for-like against the interpreter delta.
    s_shard.cycles = s0.cycles + g_runtime_shadow_cycles;

    heal_gate::StateDiff d = heal_gate::diff_full(s_interp, s_shard);
    const bool clean = d.clean && !shard_io;

    // ── Restore the live state to the interpreter result ──
    heal_gate::restore_full(s_interp, g_cpu, g_runtime_cycles, *bus);
    runtime_call_stack_restore(stack_interp.data(), depth_interp);
    g_gate_active = false;

    // ── Verdict ──
    if (clean) {
        if (++st.clean_passes >= g_threshold) {
            st.status = GateState::Status::Promoted;
            std::printf("sljit_gate: PROMOTE 0x%08X (%s) — %u consecutive clean "
                        "passes; runs native blind from here\n",
                        pc, thumb ? "thumb" : "arm", st.clean_passes);
        }
    } else {
        st.status = GateState::Status::Pinned;
        st.clean_passes = 0;
        std::printf("sljit_gate: PIN 0x%08X (%s) — shard DIVERGED from the "
                    "interpreter%s; diff-gated to the interpreter forever:\n",
                    pc, thumb ? "thumb" : "arm",
                    shard_io ? " (attempted a device write)" : "");
        for (const auto& n : d.notes)
            std::printf("sljit_gate:     %s\n", n.c_str());
    }
}

}  // namespace

bool enabled() {
    if (g_enabled < 0) resolve_enabled();
    return g_enabled == 1;
}

Decision on_dispatch(uint32_t pc, bool thumb, bool leaf, void (*shard_fn)(void)) {
    if (!leaf) return Decision::RunBlind;        // v1: only gate leaf shards
    if (g_gate_active) return Decision::RunBlind; // re-entrancy guard

    GateState& st = g_state[key(pc, thumb)];
    if (st.status == GateState::Status::Promoted) return Decision::RunBlind;
    if (st.status == GateState::Status::Pinned)   return Decision::FallThrough;

    validate(pc, thumb, shard_fn, st);  // runs interp (kept) + validates shard
    return Decision::Handled;
}

}  // namespace gbarecomp::sljit_gate
