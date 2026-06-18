// sljit_gate.cpp — see sljit_gate.h.
//
// Complete-machine-state strategy: validate a healed shard by running it FULLY
// from a snapshot of the ENTIRE machine (CPU + call-return stack + cycles + RAM
// + every device: IO/timers/DMA, PPU, audio, save) identical to the interpreter
// pass, then diffing the resulting complete state. Because the shard re-runs
// from an identical machine — pumping devices, delivering IRQs, dispatching
// callees for real — there is no shadow-tick / IO-trap / leaf restriction: IO
// functions, IRQ-crossing functions, computed jumps, switches, and non-leaf
// functions all validate by construction. The only case still pinned is a
// top-level dispatch (no pending return frame), where the shard's return idiom
// would dispatch out instead of C-returning.

#include "sljit_gate.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"          // g_cpu, g_runtime_cycles, bridge, call stack
#include "runtime_arm_types.h"    // ArmCpuState (for the divergence report)
#include "runtime_bus_bridge.h"   // active_bus / active_ppu
#include "snapshot.h"             // debug::SnapshotWriter / SnapshotReader

// IRQ-entry counter (incremented by runtime_irq on every delivery). A nonzero
// delta across the interpreter pass means an IRQ vectored mid-function; re-running
// the heavy IRQ handler in the shard's full re-run is unsafe → pin.
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

// Re-entrancy guard: while a shard's full validation run is in flight, any
// nested dispatch runs blind rather than recursively validating — the whole run
// is discarded by the restore, so nested blind execution is harmless.
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
    const long n = std::strtol(e, nullptr, 10);  // numeric > 1 → threshold
    if (n > 1) g_threshold = static_cast<unsigned>(n);
    std::printf("sljit_gate: differential gate ENABLED "
                "(promote after %u consecutive clean passes)\n", g_threshold);
}

// ── Complete machine snapshot (an in-memory save-state, no file/header) ──
// Layout: g_cpu | call-return stack | g_runtime_cycles | bus(RAM+scalars) | io |
// audio | save | ppu. The CPU is first so the divergence report can read it back
// from the blob prefix without re-snapshotting.
using Blob = std::vector<uint8_t>;

void snapshot_machine(gba::GbaBus& bus, gba::GbaPpu& ppu, Blob& out) {
    debug::SnapshotWriter w;
    w.bytes(&g_cpu, sizeof(g_cpu));
    const uint32_t depth = runtime_call_stack_depth();
    const uint32_t* data = runtime_call_stack_data();
    w.u32(depth);
    for (uint32_t i = 0; i < depth; ++i) w.u32(data[i]);
    w.u64(g_runtime_cycles);
    bus.serialize(w);
    bus.io().serialize(w);
    bus.audio().serialize(w);
    bus.save().serialize(w);
    ppu.serialize(w);
    out = w.buffer();
}

void restore_machine(gba::GbaBus& bus, gba::GbaPpu& ppu, const Blob& blob) {
    debug::SnapshotReader r(blob.data(), blob.size());
    r.bytes(&g_cpu, sizeof(g_cpu));
    const uint32_t depth = r.u32();
    std::vector<uint32_t> entries(depth, 0);
    for (uint32_t i = 0; i < depth; ++i) entries[i] = r.u32();
    runtime_call_stack_restore(entries.data(), depth);
    g_runtime_cycles = r.u64();
    bus.deserialize(r);
    bus.io().deserialize(r);
    bus.audio().deserialize(r);
    bus.save().deserialize(r);
    ppu.deserialize(r);
}

// Human-readable divergence detail from the CPU prefix of two blobs (the common
// case); a registers-match-but-blobs-differ case is RAM or device state.
void report_divergence(const Blob& a, const Blob& b) {
    if (a.size() < sizeof(ArmCpuState) || b.size() < sizeof(ArmCpuState)) {
        std::printf("sljit_gate:     (snapshot size mismatch)\n");
        return;
    }
    ArmCpuState ca, cb;
    std::memcpy(&ca, a.data(), sizeof(ca));
    std::memcpy(&cb, b.data(), sizeof(cb));
    int shown = 0;
    for (int r = 0; r < 16 && shown < 8; ++r)
        if (ca.R[r] != cb.R[r]) {
            std::printf("sljit_gate:     R[%d]: interp=0x%08X shard=0x%08X\n",
                        r, ca.R[r], cb.R[r]);
            ++shown;
        }
    if (ca.cpsr != cb.cpsr)
        std::printf("sljit_gate:     CPSR: interp=0x%08X shard=0x%08X\n",
                    ca.cpsr, cb.cpsr);
    if (shown == 0 && ca.cpsr == cb.cpsr)
        std::printf("sljit_gate:     (registers match — RAM / device state "
                    "differs)\n");
}

// Bound on the validation interp pass (a function + its subtree + at most one
// IRQ handler returns well within this; overrunning means a wrong stop).
constexpr uint64_t kGateInterpCap = 1'000'000u;

// One validation pass: interpret the function (kept, live result), then re-run
// the shard from the identical pre-call machine and diff the complete result.
// Returns Handled (the function executed, via interp) or — only when the interp
// pass overran its budget (a wrong forced stop) — FallThrough, with the pre-call
// state restored so the caller's normal dispatch path runs the function instead.
Decision validate(uint32_t pc, bool thumb, void (*fn)(void), GateState& st) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    if (!bus || !ppu) { st.status = GateState::Status::Pinned; return Decision::FallThrough; }

    const uint32_t ret_lr = g_cpu.R[14] & ~1u;

    g_gate_active = true;

    // Normalize the TRANSIENT, non-architectural bus scalars before each compared
    // snapshot: the interpreter bridge updates bios_access_enabled_/last_fetched_
    // per instruction, the shard does not, so without this they diverge the
    // snapshot spuriously. They are re-derived on the next instruction's fetch,
    // so canonicalizing them (both runs end at the same PC) loses nothing.
    auto normalize = [&]() {
        bus->set_bios_access_enabled(g_cpu.R[15] < 0x00004000u);
        bus->set_last_fetched(0u);
    };

    Blob s0;
    snapshot_machine(*bus, *ppu, s0);
    const uint64_t irq0 = g_runtime_irq_entries;

    // ── Interpreter pass — the kept, correct result (live) ──
    if (!runtime_bridge_interpret(pc, thumb, ret_lr, kGateInterpCap)) {
        // The forced LR stop was wrong (a function reached by a computed jump
        // whose LR points far up the call chain) — the pass overran. Restore the
        // pre-call state, pin, and let the caller's normal dispatch run it.
        restore_machine(*bus, *ppu, s0);
        g_gate_active = false;
        st.status = GateState::Status::Pinned;
        std::printf("sljit_gate: PIN 0x%08X (%s) — interp pass overran its budget "
                    "(unreliable stop, not validatable)\n",
                    pc, thumb ? "thumb" : "arm");
        return Decision::FallThrough;
    }

    if (g_runtime_irq_entries != irq0) {
        // An IRQ vectored during the function. Re-running its heavy handler in
        // the shard's full re-run (from restored state) is unsafe; pin. The live
        // state already IS the interpreter result.
        st.status = GateState::Status::Pinned;
        std::printf("sljit_gate: PIN 0x%08X (%s) — crossed an IRQ during the "
                    "interpreter pass (not validatable)\n",
                    pc, thumb ? "thumb" : "arm");
        g_gate_active = false;
        return Decision::Handled;
    }

    normalize();
    Blob s_interp;
    snapshot_machine(*bus, *ppu, s_interp);

    // ── Restore the pre-call machine and re-run the SHARD fully ──
    restore_machine(*bus, *ppu, s0);
    fn();
    normalize();
    Blob s_shard;
    snapshot_machine(*bus, *ppu, s_shard);

    const bool clean = (s_interp == s_shard);

    // ── Restore the live state to the interpreter result ──
    restore_machine(*bus, *ppu, s_interp);
    g_gate_active = false;

    if (clean) {
        if (++st.clean_passes >= g_threshold) {
            st.status = GateState::Status::Promoted;
            std::printf("sljit_gate: PROMOTE 0x%08X (%s) — %u consecutive clean "
                        "passes; runs native blind from here\n",
                        pc, thumb ? "thumb" : "arm", st.clean_passes);
        } else {
            std::printf("sljit_gate: clean 0x%08X (%s) — pass %u/%u\n",
                        pc, thumb ? "thumb" : "arm", st.clean_passes, g_threshold);
        }
    } else {
        st.status = GateState::Status::Pinned;
        st.clean_passes = 0;
        std::printf("sljit_gate: PIN 0x%08X (%s) — shard DIVERGED from the "
                    "interpreter; diff-gated to the interpreter forever:\n",
                    pc, thumb ? "thumb" : "arm");
        report_divergence(s_interp, s_shard);
    }
    return Decision::Handled;  // the function executed (via the interpreter pass)
}

}  // namespace

bool enabled() {
    if (g_enabled < 0) resolve_enabled();
    return g_enabled == 1;
}

Decision on_dispatch(uint32_t pc, bool thumb, bool leaf, void (*shard_fn)(void)) {
    if (g_gate_active) return Decision::RunBlind;  // re-entrancy guard

    // Gate only SELF-CONTAINED leaves (no calls / computed transfers / escaping
    // branches — see overlay_sljit_produce). The complete-machine snapshot lets
    // these validate even when they touch IO/devices (which the earlier shadow
    // gate had to pin), but running an arbitrary NON-leaf function fully during
    // validation — an IRQ handler, a deep call chain — re-does heavy side effects
    // and can overflow the host stack. Non-leaf shards run blind (unchanged).
    if (!leaf) return Decision::RunBlind;

    // Top-level dispatch (no pending return frame): its return idiom isn't a
    // C-return to LR, so it can't be cleanly validated. Run blind.
    if (runtime_call_stack_depth() == 0) return Decision::RunBlind;

    GateState& st = g_state[key(pc, thumb)];
    if (st.status == GateState::Status::Promoted) return Decision::RunBlind;
    if (st.status == GateState::Status::Pinned)   return Decision::FallThrough;

    // Handled (executed via interp), or FallThrough if the interp pass overran
    // (then the function runs via the caller's normal dispatch path).
    return validate(pc, thumb, shard_fn, st);
}

}  // namespace gbarecomp::sljit_gate
