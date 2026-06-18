// tests/sljit/gate_test.cpp — offline P6 differential-gate state machine.
//
// Deterministic, CI-friendly proof of the promote/pin logic that the in-vivo
// MinishCap run witnessed (12 live interp→sljit promotions). Drives
// sljit_gate::on_dispatch directly (it takes the shard fn, so no g_healed /
// overlay init needed) against a real GbaBus + GbaPpu + the interpreter bridge:
//   1. A CORRECT shard for a tiny leaf (add r0,#1; bx lr) diffs clean against the
//      interpreter and PROMOTES after N consecutive passes (Handled → RunBlind).
//   2. A WRONG shard (does add #2 where memory says add #1) DIVERGES and is
//      PINNED forever (Handled → FallThrough).
//
// This is the offline counterpart to the in-vivo witness — it locks the gate's
// decision logic into the test suite so a regression is caught without a full
// MinishCap build.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "arm_ir.h"
#include "arm_sljit.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "sljit_gate.h"
#include "thumb_decode.h"

using gbarecomp::sljit_gate::Decision;
using gbarecomp::sljit_gate::on_dispatch;

// The runtime references the game's cart dispatch table; this test drives
// sljit_gate directly and never dispatches through it, so an empty table
// satisfies the linker. (kBiosDispatchTable comes from gbarecomp_runtime.) The
// `extern "C"` on each definition is load-bearing: a plain `const` at namespace
// scope has internal linkage and would NOT satisfy the runtime's reference.
struct DispatchEntry { uint32_t addr; uint8_t thumb; void (*fn)(void); };
extern "C" const DispatchEntry kDispatchTable[1] = {{0xFFFFFFFFu, 0u, nullptr}};
extern "C" const unsigned kDispatchTableLen = 0u;

namespace {

int g_failures = 0;
void check(bool cond, const char* what) {
    if (!cond) { std::printf("  FAIL: %s\n", what); ++g_failures; }
}

constexpr uint32_t kLR = 0x02001000u;  // return target (outside any test fn)

// Build a shard from raw THUMB halfwords rooted at base_pc.
armv4t::SljitFn make_shard(const std::vector<uint16_t>& hw, uint32_t base_pc) {
    std::vector<armv4t::Instr> prog;
    for (std::size_t i = 0; i < hw.size(); ++i)
        prog.push_back(armv4t::ThumbDecoder::decode(hw[i],
                                                    base_pc + uint32_t(i) * 2u));
    return armv4t::emit_function_sljit(prog.data(),
                                       static_cast<unsigned>(prog.size()));
}

// Pre-call state for a leaf reached via a BL: args in r0, return addr in LR and
// on the call-return stack (so the gate doesn't treat it as a top-level miss).
void enter_call(uint32_t r0) {
    std::memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.R[0]  = r0;
    g_cpu.R[14] = kLR;
    g_cpu.cpsr  = (1u << 5) | 0x1Fu;  // THUMB + System mode
    runtime_call_stack_restore(nullptr, 0);  // clear
    runtime_call_push_return(kLR);
}

}  // namespace

int main() {
    std::printf("gate_tests: offline P6 promote/pin state machine\n");

    gba::GbaBus bus;
    gba::GbaPpu ppu;
    bus.io().set_ppu(&ppu);
    bus.io().set_bus(&bus);
    gbarecomp::set_active_bus(&bus);
    gbarecomp::set_active_ppu(&ppu);

    // ── A correct shard promotes after 3 consecutive clean passes ─────
    const uint32_t pc1 = 0x02000100u;
    bus.write16(pc1,      0x3001u);  // add r0, #1
    bus.write16(pc1 + 2u, 0x4770u);  // bx  lr
    armv4t::SljitFn good = make_shard({0x3001u, 0x4770u}, pc1);
    check(good.fn != nullptr, "correct shard emitted");

    for (int i = 1; i <= 3; ++i) {
        enter_call(/*r0=*/5);
        Decision d = on_dispatch(pc1, /*thumb=*/true, /*leaf=*/true, good.fn);
        check(d == Decision::Handled, "clean pass returns Handled (gate ran interp)");
        check(g_cpu.R[0] == 6u, "interpreter result kept live (r0 = 5+1)");
    }
    // 4th dispatch: promoted → the caller runs the shard native (RunBlind).
    enter_call(5);
    check(on_dispatch(pc1, true, true, good.fn) == Decision::RunBlind,
          "PROMOTED after 3 clean passes → RunBlind");
    // Stays promoted.
    enter_call(5);
    check(on_dispatch(pc1, true, true, good.fn) == Decision::RunBlind,
          "stays promoted on subsequent dispatches");

    // ── A divergent shard is pinned to the interpreter forever ────────
    const uint32_t pc2 = 0x02000200u;
    bus.write16(pc2,      0x3001u);  // memory says: add r0, #1
    bus.write16(pc2 + 2u, 0x4770u);  // bx lr
    armv4t::SljitFn bad = make_shard({0x3002u, 0x4770u}, pc2);  // shard does add #2
    check(bad.fn != nullptr, "divergent shard emitted");

    enter_call(5);
    check(on_dispatch(pc2, true, true, bad.fn) == Decision::Handled,
          "first divergent pass returns Handled (interp ran, shard flagged)");
    // Now pinned → the gate refuses the shard, falling through to the bridge.
    enter_call(5);
    check(on_dispatch(pc2, true, true, bad.fn) == Decision::FallThrough,
          "divergent shard PINNED → FallThrough (interpreter handles it)");
    enter_call(5);
    check(on_dispatch(pc2, true, true, bad.fn) == Decision::FallThrough,
          "stays pinned (a flaky shard never reaches budget on later passes)");

    armv4t::free_sljit_fn(good);
    armv4t::free_sljit_fn(bad);

    if (g_failures) {
        std::printf("gate_tests: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("gate_tests: all checks passed (promote + pin verified offline)\n");
    return 0;
}
