// runtime_bus_bridge.cpp — overrides the weak bus-accessor stubs in
// runtime_arm.cpp with real implementations that delegate to the
// active gba::GbaBus.
//
// The runner's main() must call `gbarecomp::set_active_bus(&bus)`
// before any recompiled cart code executes.

#include "../armv4t/runtime_arm.h"
#include "../armv4t/arm_ir.h"
#include "../gba/gba_bus.h"
#include "../gba/gba_ppu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Debug PC breakpoint (MC-HP-002 sound-engine investigation). When set
// (via the TCP `set_break_pc` command), the per-instruction
// runtime_should_yield() returns true the moment the guest PC reaches
// this address, unwinding the current runtime_dispatch back to the exec
// loop so the TCP server can inspect register/memory state. The spin we
// need to inspect (0x08004286) lives inside a single runtime_dispatch,
// so ordinary step granularity can't enter it. 0 disables (no overhead).
extern "C" uint32_t g_runtime_break_pc = 0;

// Count of PPU VBlank-start events (scanline 159->160), incremented
// unconditionally in runtime_tick regardless of DISPSTAT IRQ-enable.
// The debug step-one-frame primitive (runtime.cpp step_frame) stops when
// this advances, so the recomp's TCP `step` parks at VBlank-start — the
// same PPU phase the interpreter (tools/bios_smoke step_one_frame) and
// mGBA (runFrame) park at. Stopping at scanline-WRAP instead (the old
// ppu.frame_count() convention) parked the recomp ~68 scanlines / one
// frame of game-logic later than the oracles, manufacturing a spurious
// "recomp runs a frame ahead" when diffing memory at the same step index.
extern "C" unsigned long long g_runtime_vblank_starts = 0;

namespace gbarecomp {

static gba::GbaBus* g_active_bus = nullptr;
static gba::GbaPpu* g_active_ppu = nullptr;

// ── Guest-PC sampling profiler (debug tooling) ─────────────────────────
// A background thread samples the guest PC (g_cpu.R[15]) while the
// single-threaded interpreter core runs, building a histogram. Racy
// reads are fine for statistical sampling. Enabled by GBARECOMP_SAMPLE;
// the top hot PCs are printed to stderr at process exit. This is what
// localized the MC-HP-002 transition freeze to the M4A sound-engine
// sequence walker (0x08004286) when reasoning + per-call timing failed.
static std::thread       g_sampler;
static std::atomic<bool> g_sampling{false};
static std::unordered_map<uint32_t, uint64_t> g_pc_hist;  // sampler-thread only

static void sampler_loop() {
    while (g_sampling.load(std::memory_order_relaxed)) {
        g_pc_hist[g_cpu.R[15]]++;  // racy read; approximate is fine
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
}

static void start_sampler() {
    if (!std::getenv("GBARECOMP_SAMPLE")) return;
    if (g_sampling.exchange(true)) return;  // start once
    g_sampler = std::thread(sampler_loop);
    std::atexit([] {
        g_sampling.store(false);
        if (g_sampler.joinable()) g_sampler.join();
        std::vector<std::pair<uint32_t, uint64_t>> v(g_pc_hist.begin(),
                                                     g_pc_hist.end());
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        uint64_t total = 0;
        for (const auto& p : v) total += p.second;
        if (total == 0) return;
        std::fprintf(stderr, "[sample] %llu samples; top guest PCs:\n",
                     static_cast<unsigned long long>(total));
        for (std::size_t i = 0; i < v.size() && i < 30; ++i) {
            std::fprintf(stderr, "  0x%08X  %5.2f%%  (%llu)\n", v[i].first,
                         100.0 * static_cast<double>(v[i].second) / total,
                         static_cast<unsigned long long>(v[i].second));
        }
    });
}

bool should_trace_unmapped_read(uint32_t addr) {
    return (addr >> 24) >= 0x0Eu;
}

void trace_unmapped_read(uint32_t addr, uint32_t value, uint32_t width) {
    if (should_trace_unmapped_read(addr)) {
        runtime_trace_event(RUNTIME_TRACE_MEM_READ, g_cpu.R[15], addr, value,
                            width);
    }
}

void sync_bios_access() {
    if (g_active_bus) {
        g_active_bus->set_bios_access_enabled(g_cpu.R[15] < 0x00004000u);
    }
}

void set_active_bus(gba::GbaBus* bus) {
    g_active_bus = bus;
    start_sampler();  // no-op unless GBARECOMP_SAMPLE is set
}

void set_active_ppu(gba::GbaPpu* ppu) {
    g_active_ppu = ppu;
}

gba::GbaBus* active_bus() {
    return g_active_bus;
}

}  // namespace gbarecomp

extern "C" uint32_t bus_read_u32(uint32_t addr) {
    gbarecomp::sync_bios_access();
    uint32_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read32(addr)
        : 0u;
    gbarecomp::trace_unmapped_read(addr, v, 4u);
    return v;
}

extern "C" uint16_t bus_read_u16(uint32_t addr) {
    gbarecomp::sync_bios_access();
    uint16_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read16(addr)
        : uint16_t{0};
    gbarecomp::trace_unmapped_read(addr, v, 2u);
    return v;
}

extern "C" uint8_t bus_read_u8(uint32_t addr) {
    gbarecomp::sync_bios_access();
    uint8_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read8(addr)
        : uint8_t{0};
    gbarecomp::trace_unmapped_read(addr, v, 1u);
    return v;
}

extern "C" void bus_write_u32(uint32_t addr, uint32_t val) {
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write32(addr, val);
}

extern "C" void bus_write_u16(uint32_t addr, uint16_t val) {
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write16(addr, val);
}

extern "C" void bus_write_u8(uint32_t addr, uint8_t val) {
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write8(addr, val);
}

extern "C" uint32_t runtime_mem_cycles(uint32_t addr, uint32_t width,
                                       uint32_t sequential) {
    auto* bus = gbarecomp::g_active_bus;
    return bus ? bus->access_cycles(addr, static_cast<uint8_t>(width),
                                    sequential != 0u)
               : 1u;
}

extern "C" uint32_t runtime_mul_cycles(uint32_t rs_value,
                                       uint32_t signed_variant,
                                       uint32_t extra) {
    return armv4t::mul_wait_cycles(rs_value, signed_variant != 0u, extra);
}

extern "C" void runtime_tick(uint32_t cycles) {
    auto* bus = gbarecomp::g_active_bus;
    auto* ppu = gbarecomp::g_active_ppu;
    if (!bus || !ppu || cycles == 0) return;

    uint32_t remaining = cycles;
    while (remaining != 0) {
        uint32_t chunk = remaining;
        uint32_t until_sample = bus->audio().cycles_until_next_sample();
        uint32_t until_timer = bus->io().cycles_until_next_timer_event();
        uint32_t until_ppu = ppu->cycles_until_next_event();
        if (until_sample < chunk) chunk = until_sample;
        if (until_timer < chunk) chunk = until_timer;
        if (until_ppu < chunk) chunk = until_ppu;
        if (chunk == 0) chunk = 1;

        bus->audio().tick(chunk);
        bus->io().tick_timers(chunk);

        uint16_t vc_compare = static_cast<uint16_t>(
            (bus->io().dispstat() >> 8) & 0xFFu);
        auto events = ppu->tick(chunk, vc_compare);
        uint16_t ds = bus->io().dispstat();
        if (events.hblank_started &&
            ppu->vcount() < gba::GbaPpu::kLinesVisible) {
            ppu->render_scanline(ppu->vcount(),
                                 bus->io().read16(0x000),
                                 bus->io().raw(),
                                 bus->vram_ptr(),
                                 bus->oam_ptr(),
                                 bus->pal_ptr());
        }
        if (events.vblank_started) {
            ppu->mark_framebuffer_latched();
            ++g_runtime_vblank_starts;
        }
        if (events.vblank_started && (ds & 0x0008u)) {
            bus->io().request_irq(gba::GbaIo::IrqVBlank);
        }
        if (events.hblank_started && (ds & 0x0010u)) {
            bus->io().request_irq(gba::GbaIo::IrqHBlank);
        }
        if (events.vcount_matched && (ds & 0x0020u)) {
            bus->io().request_irq(gba::GbaIo::IrqVCount);
        }

        remaining -= chunk;
    }

    if (bus->io().irq_pending() && (g_cpu.cpsr & CPSR_I_BIT) == 0) {
        if (bus->io().halted()) bus->io().clear_halt();
        runtime_irq(g_cpu.R[15]);
    }
}

extern "C" bool runtime_should_yield(void) {
    if (g_runtime_break_pc != 0u && g_cpu.R[15] == g_runtime_break_pc) {
        return true;  // debug breakpoint hit — unwind to the exec loop
    }
    auto* bus = gbarecomp::g_active_bus;
    return bus && bus->io().halted();
}
