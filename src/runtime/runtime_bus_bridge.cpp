// runtime_bus_bridge.cpp — overrides the weak bus-accessor stubs in
// runtime_arm.cpp with real implementations that delegate to the
// active gba::GbaBus.
//
// The runner's main() must call `gbarecomp::set_active_bus(&bus)`
// before any recompiled cart code executes.

#include "../armv4t/runtime_arm.h"
#include "../armv4t/arm_ir.h"
#include "../gba/gba_bus.h"
#include "../gba/gba_irq.h"
#include "../gba/gba_m4a.h"
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

// Cumulative guest-cycle clock (MC-HP-002 cycle-aligned divergence hunt).
// Incremented by runtime_tick on EVERY tick — both the per-instruction exec
// ticks emitted by generated code and the halt-pump chunks — so it is the
// authoritative total guest-cycle count. (runtime.cpp's `cycles_elapsed` only
// summed the halt path, which is why it was incomparable to the interpreter's
// fixed-quantum clock.) runtime_trace_event stamps this onto every ring entry
// so the recomp and the bios_smoke interp oracle align by identical cycles.
extern "C" unsigned long long g_runtime_cycles = 0;

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
        // Stash the live PC so an unmapped (open-bus) read returns the CURRENT
        // prefetch of the executing code (GBATEK open-bus). MC-HP-002.
        g_active_bus->note_pc(g_cpu.R[15], (g_cpu.cpsr & CPSR_T_BIT) != 0);
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

// Always-on hang watchdog capture. Called once when the watchdog trips;
// snapshots the live MP2K channel state (the MC-HP-002 spin walks a
// corrupt voice pointer) to stderr and hang_dump.log next to the runner.
// Pure observation — does not alter execution. See PRINCIPLES.md
// "always-on ring first": the freeze documents itself, no attach timing.
static void dump_hang_state(const char* reason) {
    if (!g_active_bus) return;
    std::string m4a;
    gba::mp2k_dump_live(*g_active_bus, m4a);

    // Full CPU register snapshot — the hang is a busy loop, so r0..r15 pin the
    // object/pointer it is walking (MC-HP-002: UpdateAnimationVariableFrames
    // walks a corrupt frame-list pointer r1 = *(animObj+0x5c)). Also dump the
    // candidate animation object (r0) and the word it loads at +0x5c so the
    // corrupt value is attributable without a second run.
    char regs[512];
    int n = 0;
    for (int i = 0; i < 16; ++i) {
        n += std::snprintf(regs + n, sizeof(regs) - n, "r%d=0x%08X ",
                           i, g_cpu.R[i]);
    }
    uint32_t r0 = g_cpu.R[0];
    uint32_t cmdptr = g_active_bus->read32(r0 + 0x5cu);
    char objbuf[256];
    int m = std::snprintf(objbuf, sizeof(objbuf),
                          "obj@r0=0x%08X  *(r0+0x5c)=0x%08X  obj[0x00..0x60]:",
                          r0, cmdptr);
    for (uint32_t off = 0; off < 0x60u && m < (int)sizeof(objbuf) - 4; off += 4) {
        m += std::snprintf(objbuf + m, sizeof(objbuf) - m, " %08X",
                           g_active_bus->read32(r0 + off));
    }

    std::fprintf(stderr,
                 "\n[hang-watchdog] %s\n  pc=0x%08X cpsr=0x%08X cycles=%llu "
                 "vblank_starts=%llu\n  %s\n  %s\n  m4a=%s\n",
                 reason, g_cpu.R[15], g_cpu.cpsr,
                 static_cast<unsigned long long>(g_runtime_cycles),
                 static_cast<unsigned long long>(g_runtime_vblank_starts),
                 regs, objbuf, m4a.c_str());
    if (FILE* f = std::fopen("hang_dump.log", "w")) {
        std::fprintf(f,
                     "reason=%s\npc=0x%08X\ncpsr=0x%08X\ncycles=%llu\n"
                     "vblank_starts=%llu\nregs=%s\n%s\nm4a=%s\n",
                     reason, g_cpu.R[15], g_cpu.cpsr,
                     static_cast<unsigned long long>(g_runtime_cycles),
                     static_cast<unsigned long long>(g_runtime_vblank_starts),
                     regs, objbuf, m4a.c_str());
        std::fclose(f);
    }

    // Self-documenting execution history: dump the tail of the always-on
    // per-instruction fingerprint ring so the PCs the game thread executed
    // leading INTO the freeze are on disk (PRINCIPLES.md "always-on ring
    // first" — query the ring for the window, never arm-then-capture). This
    // is what distinguishes a native cart spin (a small cycling PC set) from a
    // marching pointer walk (monotonic PC/addr) from an interp-bridge over RAM
    // code (PCs in 0x02/0x03) WITHOUT a second run. Requires GBARECOMP_INSN_TRACE
    // armed at launch; harmlessly writes 0 records if the ring is empty.
    uint32_t fpn = runtime_fp_save_tail_csv("hang_fp_tail.csv", 16384);
    std::fprintf(stderr, "[hang-watchdog] wrote hang_fp_tail.csv (%u records); "
                 "arm GBARECOMP_INSN_TRACE=1 if 0.\n", fpn);

    // Recent mem-write/branch trace ring (ALWAYS-ON — emitted unconditionally by
    // codegen, independent of GBARECOMP_INSN_TRACE). A busy-spin freeze loop
    // performs only reads, so this ring is FROZEN at the instant the spin began:
    // it still holds the WRITES that produced the corrupt state the loop chokes
    // on (e.g. whoever wrote the bad pointer being walked). Dumping it makes the
    // writer of a wrong value attributable from the freeze alone — trace-the-
    // writer without a second run (PRINCIPLES.md "find the first divergence").
    {
        static RuntimeTraceEntry tr[4096];
        uint32_t ntr = runtime_trace_copy_recent(tr, 4096);
        if (FILE* tf = std::fopen("hang_trace.csv", "w")) {
            std::fprintf(tf, "seq,cycles,kind,pc,addr,value,aux,"
                             "r0,r1,r2,r3,r4,r5,r12,sp,lr\n");
            for (uint32_t i = 0; i < ntr; ++i) {
                const RuntimeTraceEntry& e = tr[i];
                std::fprintf(tf,
                    "%u,%llu,%u,0x%08X,0x%08X,0x%08X,0x%X,"
                    "0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
                    e.seq, static_cast<unsigned long long>(e.cycles), e.kind,
                    e.pc, e.addr, e.value, e.aux, e.r0, e.r1, e.r2, e.r3,
                    e.r4, e.r5, e.r12, e.r13, e.r14);
            }
            std::fclose(tf);
            std::fprintf(stderr,
                "[hang-watchdog] wrote hang_trace.csv (%u mem-write/branch "
                "records — frozen at spin onset; find the corrupt-value writer "
                "here).\n", ntr);
        }
    }
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

// Advance the devices (audio, timers, PPU) by `cycles`, rendering scanlines
// and raising IRQ-pending bits (IF) as events occur — but WITHOUT taking the
// IRQ. Shared by runtime_tick's normal path and the wake-from-HALT latency
// pump, so the delay window ticks devices identically without re-vectoring.
static void tick_devices(gba::GbaBus* bus, gba::GbaPpu* ppu, uint32_t cycles) {
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
            // HBlank-timed DMA fires on each visible-line HBlank — AFTER the
            // line is rendered, so line N used the value the previous HBlank's
            // DMA loaded (matching the HBlank-IRQ ordering below and hardware:
            // the DMA at HBlank N loads the register used by line N+1). This is
            // what walks the per-scanline WIN0H circle table for the transition
            // iris (MC-HP-003).
            bus->io().run_timed_dma(2);
        }
        if (events.vblank_started) {
            ppu->mark_framebuffer_latched();
            ++g_runtime_vblank_starts;
            bus->io().run_timed_dma(1);   // VBlank-timed DMA
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
}

extern "C" void runtime_tick(uint32_t cycles) {
    auto* bus = gbarecomp::g_active_bus;
    auto* ppu = gbarecomp::g_active_ppu;
    if (!bus || !ppu || cycles == 0) return;

    g_runtime_cycles += cycles;
    tick_devices(bus, ppu, cycles);

    if (bus->io().irq_pending() && (g_cpu.cpsr & CPSR_I_BIT) == 0) {
        g_runtime_irq_from_halt = bus->io().halted() ? 1u : 0u;
        if (bus->io().halted()) {
            // Wake-from-HALT IRQ latency. The ARM7TDMI does not vector the
            // instant the IRQ pends out of HALT, and the interpreter oracle
            // (bios_smoke) models this delay. Taking it immediately here
            // vectored ~kIrqWakeDelayCycles early every VBlank (the game
            // VBlankIntrWaits each frame), shifting m4a sequencer phase enough
            // to double-tick a channel once a fade started → MC-HP-002 hang.
            // Pump the devices for the latency window WITHOUT re-taking the
            // IRQ (tick_devices, not runtime_tick), then vector — matching the
            // oracle exactly. Newly-pended bits stay in IF for the next check.
            // These wake-delay cycles really elapse on hardware, so they must
            // also advance the cumulative cycle clock (the oracle's pump_step
            // counts them in cycles_elapsed). Omitting this left g_runtime_cycles
            // 7 cyc/IRQ short of the oracle, growing the fingerprint cycle skew
            // by -7/IRQ (the MC-HP-002 cycle drift) — cosmetic for execution
            // (the PPU/audio above are ticked regardless) but it broke cycle-
            // aligned diffing.
            bus->io().clear_halt();
            g_runtime_cycles += gba::kIrqWakeDelayCycles;
            tick_devices(bus, ppu, gba::kIrqWakeDelayCycles);
        }
        runtime_irq(g_cpu.R[15]);
    }
}

extern "C" bool runtime_should_yield(void) {
    auto* bus = gbarecomp::g_active_bus;

    // ── BIOS open-bus prefetch latch (MC-HP-002) ─────────────────────────
    // Called once per guest instruction (the generated prologue calls this with
    // g_cpu.R[15] == the current PC). While PC is in the BIOS, keep the bus's
    // biosPrefetch latch current; when the BIOS hands control to the cart it
    // then holds the prefetch at the exit instruction — exactly what a later
    // cart read of the protected BIOS region must return (mGBA biosPrefetch;
    // for the GBA SWI return at BIOS 0x188 that is mem[0x190]=0xE3A02004). A
    // single compare per instruction; the image read happens only inside the
    // BIOS. See gba_bus.cpp prefetch_word / the open-bus read paths.
    if (bus && g_cpu.R[15] < 0x00004000u)
        bus->latch_bios_prefetch(g_cpu.R[15], (g_cpu.cpsr & CPSR_T_BIT) != 0);

    bool halted = bus && bus->io().halted();

    // ── Always-on hang watchdog ──────────────────────────────────────
    // A healthy GBA game HALTs (VBlankIntrWait) ~60x/sec; the MC-HP-002
    // freeze is a busy-spin in the M4A mixer that NEVER halts (the PPU
    // still ticks, so frame counters keep advancing — "no frames" is the
    // wrong signal; "no HALT" is the right one). If we go many seconds of
    // wall-clock with no HALT, snapshot the live M4A state ONCE and keep
    // running (observation, not a fix). Disable with
    // GBARECOMP_HANG_WATCHDOG=0; tune seconds with GBARECOMP_HANG_SECONDS.
    static const bool wd_on = [] {
        const char* e = std::getenv("GBARECOMP_HANG_WATCHDOG");
        return !(e && e[0] == '0' && e[1] == '\0');
    }();
    if (wd_on) {
        static const long long wd_secs = [] {
            const char* e = std::getenv("GBARECOMP_HANG_SECONDS");
            long long s = e ? std::atoll(e) : 0;
            return s > 0 ? s : 4;
        }();
        static bool tripped = false;
        static std::chrono::steady_clock::time_point last_halt =
            std::chrono::steady_clock::now();
        static unsigned long long calls = 0;
        if (halted) {
            last_halt = std::chrono::steady_clock::now();
        } else if (!tripped && (++calls & 0xFFFFFull) == 0) {
            auto idle = std::chrono::steady_clock::now() - last_halt;
            if (std::chrono::duration_cast<std::chrono::seconds>(idle).count() >= wd_secs) {
                gbarecomp::dump_hang_state(
                    "guest has not HALTed for several seconds — likely a busy-spin "
                    "freeze (MC-HP-002 class)");
                tripped = true;
            }
        }
    }

    if (g_runtime_break_pc != 0u && g_cpu.R[15] == g_runtime_break_pc) {
        return true;  // debug breakpoint hit — unwind to the exec loop
    }
    return halted;
}
