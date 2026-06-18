// sljit_gate.h — P6 differential gate.
//
// Before an sljit-healed shard is trusted to run BLIND in normal play, validate
// it against the interpreter from the SAME pre-call state: run the interpreter
// pass (the kept, correct result), then re-run the shard in shadow mode from the
// snapshot and diff their effects (registers + CPSR + banked + cycles + RAM). A
// shard promotes to run-blind only after N consecutive 0-divergence passes; ANY
// divergence pins it to the interpreter forever (so an intermittently-wrong
// shard never reaches budget on lucky passes). Gated behind
// GBARECOMP_HEAL_SLJIT_LIVE — default off, so the gcc path and normal sljit play
// are unchanged.
//
// v1 scope (safe + tractable): only LEAF shards (no calls) are gated — they
// can't recurse into the gate or perturb the caller's subtree, and SWI is
// emitter-declined so a shard never re-runs a BIOS SWI. Functions that touch IO
// or cross an IRQ delivery during the interpreter pass are pinned (their side
// effects can't be cleanly shadow-replayed). Non-leaf shards run blind exactly
// as they do today (the gate is purely additive safety, never a slowdown).

#pragma once

#include <cstdint>

namespace gbarecomp::sljit_gate {

// True when GBARECOMP_HEAL_SLJIT_LIVE is truthy (resolved once on first query).
bool enabled();

enum class Decision {
    RunBlind,     // promoted, or not gate-eligible → caller runs shard.fn()
    Handled,      // gate ran the interpreter pass (kept result) + validated the
                  // shard → caller must NOT run the shard; dispatch is satisfied
    FallThrough,  // pinned → caller returns 0 so the interpreter bridge runs it
};

// Decide+act for a healed sljit entry at dispatch. `leaf` = the function makes
// no calls (gate-eligible in v1). `shard_fn` is the shard entry.
Decision on_dispatch(uint32_t pc, bool thumb, bool leaf, void (*shard_fn)(void));

}  // namespace gbarecomp::sljit_gate
