// tests/heal_gate/heal_gate_test.cpp — P6 gate primitives, offline.
//
// Validates the full-RAM snapshot/restore/diff (the obviously-correct reference
// strategy the journal will later be cross-checked against):
//   1. capture → mutate cpu+RAM → restore → re-capture diffs CLEAN.
//   2. diff_full FLAGS a known divergence (register, CPSR, RAM byte, cycles).
//   3. an unmutated round-trip stays clean (no false positives).
//
// Links only gbarecomp_gba (GbaBus) — the primitives are pure (no runtime
// globals), so no ROM/BIOS/runtime is needed.

#include <cstdint>
#include <cstdio>

#include "gba_bus.h"
#include "heal_gate.h"
#include "runtime_arm_types.h"

using namespace gbarecomp::heal_gate;

namespace {

int g_failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

constexpr uint32_t kEwram = 0x02000000u;  // 256 KB
constexpr uint32_t kIwram = 0x03000000u;  //  32 KB

}  // namespace

int main() {
    std::printf("heal_gate_tests: full-RAM snapshot/restore/diff\n");

    gba::GbaBus bus;
    ArmCpuState cpu{};
    uint64_t cycles = 0;

    // ── Seed a pre-call state ────────────────────────────────────────
    cpu.R[0] = 0x00000111u;
    cpu.R[13] = 0x03007F00u;
    cpu.cpsr = 0x0000001Fu;  // System mode
    bus.write32(kEwram + 0x40u, 0x11223344u);
    bus.write32(kIwram + 0x10u, 0xAABBCCDDu);
    cycles = 1000;

    StateSnapshot snap0 = capture_full(cpu, cycles, bus);

    // ── Mutate everything a function might touch ──────────────────────
    cpu.R[0] = 0x00000222u;
    cpu.R[14] = 0x08001000u;
    cpu.cpsr = 0x00000013u;  // Supervisor mode
    bus.write32(kEwram + 0x40u, 0xDEADBEEFu);
    bus.write32(kIwram + 0x10u, 0x00000000u);
    bus.write32(kEwram + 0x800u, 0xFEEDFACEu);  // a fresh location too
    cycles = 1057;

    StateSnapshot snapA = capture_full(cpu, cycles, bus);

    // diff(pre, post-mutation) must be DIRTY and name the divergences.
    StateDiff dirty = diff_full(snap0, snapA);
    check(!dirty.clean, "diff flags a mutated state as dirty");
    check(!dirty.notes.empty(), "dirty diff carries divergence notes");
    // Spot-check that distinct classes were caught (register, cpsr, ram, cycles).
    bool saw_reg = false, saw_cpsr = false, saw_ram = false, saw_cyc = false;
    for (const auto& n : dirty.notes) {
        if (n.rfind("R[", 0) == 0) saw_reg = true;
        if (n.rfind("CPSR", 0) == 0) saw_cpsr = true;
        if (n.rfind("EWRAM", 0) == 0 || n.rfind("IWRAM", 0) == 0) saw_ram = true;
        if (n.rfind("cycles", 0) == 0) saw_cyc = true;
    }
    check(saw_reg, "diff names a register divergence");
    check(saw_cpsr, "diff names a CPSR divergence");
    check(saw_ram, "diff names a RAM divergence");
    check(saw_cyc, "diff names a cycle divergence");

    // ── Restore to the pre-call state and confirm it round-trips clean ─
    restore_full(snap0, cpu, cycles, bus);
    StateSnapshot snapB = capture_full(cpu, cycles, bus);
    StateDiff clean = diff_full(snap0, snapB);
    check(clean.clean, "restore round-trips to a clean diff");
    check(cpu.R[0] == 0x00000111u, "restore brought R0 back");
    check(cpu.cpsr == 0x0000001Fu, "restore brought CPSR back");
    check(cycles == 1000u, "restore brought the cycle counter back");
    check(bus.read32(kEwram + 0x40u) == 0x11223344u, "restore brought EWRAM back");
    check(bus.read32(kIwram + 0x10u) == 0xAABBCCDDu, "restore brought IWRAM back");
    check(bus.read32(kEwram + 0x800u) == 0u, "restore undid the fresh EWRAM write");

    // ── Identity: capturing twice with no change diffs clean ──────────
    StateSnapshot snapC = capture_full(cpu, cycles, bus);
    check(diff_full(snapB, snapC).clean, "identical captures diff clean");

    // ── Journal strategy: record → rollback round-trips byte-for-byte.
    //    The full-RAM diff is the oracle: a clean round-trip PROVES the
    //    journal captured every write (a missed write wouldn't restore). ─
    {
        Journal jrn;
        jrn.arm(bus, Journal::Mode::Record);
        StateSnapshot j0 = capture_full(cpu, cycles, bus);

        // Varied: several regions + widths + an OVERLAPPING double write to one
        // address (rollback must restore the ORIGINAL, not the intermediate).
        bus.write32(kEwram + 0x100u, 0x01020304u);
        bus.write16(kEwram + 0x100u, 0x0000BEEFu);     // overlaps the low half
        bus.write8 (kIwram + 0x004u, 0x0000007Fu);
        bus.write32(0x05000000u + 0x20u, 0x0000FFFFu); // PAL
        bus.write16(0x06000000u + 0x40u, 0x00001234u); // VRAM
        bus.write32(0x07000000u + 0x08u, 0xCAFEBABEu); // OAM

        StateSnapshot j1 = capture_full(cpu, cycles, bus);
        check(!diff_full(j0, j1).clean, "journal: writes mutate RAM");
        check(jrn.write_count() >= 6, "journal recorded the RAM writes");
        check(!jrn.io_touched(), "journal: pure RAM writes are not device-touch");

        jrn.rollback(bus);
        jrn.disarm(bus);
        StateSnapshot j2 = capture_full(cpu, cycles, bus);
        check(diff_full(j0, j2).clean,
              "journal rollback restores RAM byte-for-byte (vs full-RAM oracle)");
    }

    // ── Journal SHADOW mode traps device (IO) writes ─────────────────
    {
        constexpr uint32_t kDispcnt = 0x04000000u;
        Journal rec;
        rec.arm(bus, Journal::Mode::Record);
        bus.write16(kDispcnt, 0x00000100u);  // applies in RECORD mode
        rec.disarm(bus);
        check(rec.io_touched(), "journal RECORD flags an IO write as device-touch");
        uint16_t before = bus.read16(kDispcnt);

        Journal shd;
        shd.arm(bus, Journal::Mode::Shadow);
        bus.write16(kDispcnt, 0x00000040u);  // must be TRAPPED (not applied)
        shd.disarm(bus);
        uint16_t after = bus.read16(kDispcnt);
        check(shd.io_touched(), "journal SHADOW flags the device write");
        check(after == before, "journal SHADOW traps (suppresses) the device write");
    }

    // ── Observer fully disarmed: normal writes are untouched ─────────
    {
        bus.write32(kEwram + 0x200u, 0x99887766u);
        check(bus.read32(kEwram + 0x200u) == 0x99887766u,
              "writes apply normally when no observer is armed");
    }

    if (g_failures) {
        std::printf("heal_gate_tests: %d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
    std::printf("heal_gate_tests: all checks passed\n");
    return 0;
}
