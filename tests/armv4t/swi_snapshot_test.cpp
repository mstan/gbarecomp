#include "runtime_context.h"
#include "snapshot.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include <memory>
#include <cstdio>

int main() {
    auto bus = std::make_unique<gba::GbaBus>();
    auto ppu = std::make_unique<gba::GbaPpu>();
    gbarecomp::debug::SnapshotContext ctx;
    ctx.bus = bus.get(); ctx.ppu = ppu.get();
    ctx.rom_sha1 = "0123456789012345678901234567890123456789";
    runtime_init(nullptr);
    g_cpu = {};
    g_cpu.cpsr = 0x13;
    g_cpu.banked_spsr[ARM_BANK_SUPERVISOR] = 0x3f;
    gbarecomp::runtime_restore_swi_context({{0x08001002u, 0x08001000u}});
    std::string error;
    std::vector<uint8_t> bytes;
    if (!gbarecomp::debug::save_state_bytes(&bytes, ctx, &error)) return 1;
    gbarecomp::runtime_restore_swi_context({});
    if (!gbarecomp::debug::load_state_bytes(bytes.data(), bytes.size(), ctx, &error)) return 2;
    runtime_fp_reset();
    std::vector<gbarecomp::RuntimeSwiContinuation> restored;
    gbarecomp::runtime_capture_swi_context(restored);
    if (restored.size() != 1 || restored[0].continuation != 0x08001002u) return 3;
    // CPU0 is the first section. Its active SWI count follows the raw CPU
    // state and the empty call-return stack. Reject it before changing CPU.
    auto invalid = bytes;
    const std::size_t count = 52u + 8u + sizeof(ArmCpuState) + 4u;
    invalid[count] = 65;
    g_cpu.R[0] = 12345;
    if (gbarecomp::debug::load_state_bytes(invalid.data(), invalid.size(), ctx, &error) ||
        g_cpu.R[0] != 12345) return 4;
    runtime_exception_return(0x08001002u);
    if (g_cpu.R[15] != 0x08001002u) return 5;
    std::puts("SVC snapshot roundtrip and invalid-depth rejection passed");
}
