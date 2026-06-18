// heal_gate.h — P6 differential-gate primitives (snapshot / restore / diff).
//
// Before an sljit-healed shard is trusted to run live, the gate runs it AND the
// interpreter from the SAME pre-call state and compares their effects. Because a
// v1 sljit shard bakes the real &g_cpu / &bus_write_* addresses, it mutates the
// live machine in place — so "same-state replay" is a save → run → restore →
// run → diff dance against the real state. These primitives provide the
// save/restore/diff half of that dance; the gate STATE MACHINE (consecutive-
// clean budget, device-touch pinning, interp-first orchestration) lives one
// layer up in gbarecomp_runtime, which has the overlay/heal hooks.
//
// Two interchangeable snapshot strategies (selectable so they cross-check each
// other — the obviously-correct one validates the fast one):
//   * FULL-RAM  — copy the five writable RAM regions verbatim. No bus hook,
//                 obviously correct, ~386 KB per capture. The reference.
//   * JOURNAL   — record only the writes a pass makes (at the GbaBus write
//                 chokepoint) and roll back by replaying the inverse. Cost is
//                 proportional to the function's actual write footprint. (Built
//                 in a later step; this header reserves its types.)
//
// This module is PURE: it depends only on ArmCpuState (the CPU file) and
// gba::GbaBus, never on the runtime globals (g_cpu / g_runtime_cycles). The gate
// passes those in. That keeps it unit-testable by linking just gbarecomp_gba.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gba_bus.h"            // gba::GbaBus, BusWriteObserver, BusWriteRegion
#include "runtime_arm_types.h"  // ArmCpuState

namespace gbarecomp::heal_gate {

// The five writable RAM regions a healed function can mutate (mirrors the
// GbaBus write chokepoint's Region cases; IO / Save / RTC are device-touch and
// handled separately by the gate, never journaled as plain memory).
enum class GateRegion : uint8_t { Ewram, Iwram, Pal, Vram, Oam };
constexpr int kGateRegionCount = 5;

// A captured snapshot of the mutable machine state needed to replay a function
// from identical state and diff its effects. For the FULL-RAM strategy every
// region vector is populated; the JOURNAL strategy leaves them empty (the
// writes live in a Journal instead) and carries only cpu + cycles.
struct StateSnapshot {
    ArmCpuState cpu{};
    uint64_t    cycles = 0;
    std::vector<uint8_t> ewram, iwram, pal, vram, oam;  // full-RAM copies
};

// Capture cpu + cycles + all five RAM regions from `bus` (full-RAM strategy).
StateSnapshot capture_full(const ArmCpuState& cpu, uint64_t cycles,
                           const gba::GbaBus& bus);

// Restore cpu + cycles + all five RAM regions back into `bus` (full-RAM).
void restore_full(const StateSnapshot& s, ArmCpuState& cpu, uint64_t& cycles,
                  gba::GbaBus& bus);

// A structured diff between two post-run states (shard vs interpreter). `notes`
// carries the first few human-readable divergences for the loud heal log.
struct StateDiff {
    bool clean = true;
    std::vector<std::string> notes;
};

// Compare two full-RAM snapshots: R0..R15 + CPSR + banked state + ticked cycles
// + every RAM region. Caps `notes` so a wildly-divergent shard doesn't flood.
StateDiff diff_full(const StateSnapshot& a, const StateSnapshot& b);

// ── Journal strategy ──────────────────────────────────────────────────────
// Records the RAM writes a pass makes (at the GbaBus write chokepoint, via the
// BusWriteObserver hook) so the pass can be rolled back by replaying the inverse
// — cost is proportional to the function's actual write footprint, not the
// 386 KB of total RAM. The full-RAM strategy is its correctness oracle: a
// record → rollback round-trip must restore byte-for-byte (the offline test
// asserts exactly that), which proves no write was missed.
class Journal : public gba::BusWriteObserver {
public:
    // RECORD: journal RAM writes and apply EVERYTHING (the interpreter pass —
    //         its result is kept live, including device/IO side effects).
    // SHADOW: journal RAM writes but TRAP device writes (don't apply) — the
    //         shard validation pass, so a mis-compiled shard can't fire a stray
    //         IO/DMA side effect during validation.
    enum class Mode { Record, Shadow };

    void arm(gba::GbaBus& bus, Mode mode);  // register as the bus's observer
    void disarm(gba::GbaBus& bus);          // clear the bus's observer
    void clear();                           // forget recorded writes + flags

    bool on_bus_write(gba::BusWriteRegion region, uint32_t off, uint32_t addr,
                      uint8_t width, uint32_t old_value,
                      uint32_t new_value) override;

    // Undo the recorded RAM writes — reverse order, writing each old value back
    // directly to the region arrays (NOT via bus.write*, which would re-journal).
    void rollback(gba::GbaBus& bus) const;

    bool        io_touched() const { return io_touched_; }
    uint32_t    io_first_addr() const { return io_first_addr_; }
    std::size_t write_count() const { return ram_writes_.size(); }

    struct Entry {
        gba::BusWriteRegion region;
        uint8_t  width;
        uint32_t off;
        uint32_t old_value;
        uint32_t new_value;
    };
    const std::vector<Entry>& entries() const { return ram_writes_; }

private:
    std::vector<Entry> ram_writes_;
    bool     io_touched_    = false;
    uint32_t io_first_addr_ = 0;
    Mode     mode_          = Mode::Record;
};

}  // namespace gbarecomp::heal_gate
