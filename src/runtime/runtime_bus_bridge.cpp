// runtime_bus_bridge.cpp — overrides the weak bus-accessor stubs in
// runtime_arm.cpp with real implementations that delegate to the
// active gba::GbaBus.
//
// The runner's main() must call `gbarecomp::set_active_bus(&bus)`
// before any recompiled cart code executes.

#include "../armv4t/runtime_arm.h"
#include "../armv4t/arm_ir.h"
#include "../armv4t/symbol_lookup.h"
#include "../gba/gba_bus.h"
#include "../gba/gba_irq.h"
#include "../gba/gba_m4a.h"
#include "../gba/gba_ppu.h"

#ifdef GBA_COSIM
#include "../debug/cosim.h"   // cosim_on_tick() — first-divergence checkpoint hook
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
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

// ── Phase profiler (GBARECOMP_PHASE_PROF=1) ─────────────────────────────────
// De-confounds the guest-PC sampler's biggest attribution trap: PPU pixel
// composition runs per-visible-scanline inside tick_devices, which runs inside
// runtime_tick — called from EVERY recompiled guest instruction, including the
// WaitForVBlank busy-spin. A guest-PC profiler therefore charges composition to
// whatever PC was live (usually the spin). Timing render_scanline directly
// measures composition's true wall-time share. ~160 timed calls/frame, so the
// steady_clock overhead is negligible; fully gated off (no clock reads) unless
// the env var is set. Dumped to stderr at exit.
extern "C" unsigned long long g_prof_scanline_ns = 0;
extern "C" unsigned long long g_prof_scanline_count = 0;
static const bool g_phase_prof = [] {
    const char* e = std::getenv("GBARECOMP_PHASE_PROF");
    bool on = (e != nullptr) && !(e[0] == '0' && e[1] == '\0');
    if (on) {
        std::atexit([] {
            std::fprintf(stderr,
                "[phase] render_scanline: %llu ns over %llu calls "
                "(avg %.1f ns/call)\n",
                g_prof_scanline_ns, g_prof_scanline_count,
                g_prof_scanline_count
                    ? static_cast<double>(g_prof_scanline_ns) /
                          static_cast<double>(g_prof_scanline_count)
                    : 0.0);
        });
    }
    return on;
}();
static unsigned long long g_runtime_yielded_vblank_start = 0;
// Live IRQ-handler nesting depth (defined in armv4t/runtime_arm.cpp: ++ on IRQ
// entry, -- after the handler unwinds). Used by the vblank-yield guard below to
// avoid yielding while an IRQ handler is on the host stack.
extern "C" uint32_t g_irq_nest_depth;

// Cumulative guest-cycle clock (MC-HP-002 cycle-aligned divergence hunt).
// Incremented by runtime_tick on EVERY tick — both the per-instruction exec
// ticks emitted by generated code and the halt-pump chunks — so it is the
// authoritative total guest-cycle count. (runtime.cpp's `cycles_elapsed` only
// summed the halt path, which is why it was incomparable to the interpreter's
// fixed-quantum clock.) runtime_trace_event stamps this onto every ring entry
// so the recomp and the bios_smoke interp oracle align by identical cycles.
extern "C" unsigned long long g_runtime_cycles = 0;

// ── Stage 2 idle-loop elision: disturbance epoch + skip counters ─────────────
// Bumped whenever something happens that could change a watched poll value or
// the timing rules: a guest memory write, an MMIO read (disqualifies MMIO
// polling), or a device-event materialization (tick_devices). The per-site
// prover (runtime_idle_backedge) requires it unchanged across the two proof
// iterations. See runtime_arm.h.
extern "C" unsigned long long g_idle_disturb_epoch = 0;
extern "C" unsigned long long g_runtime_state_epoch = 0;
// Diagnostics for the exit banner: cycles/iterations the prover fast-forwarded.
static unsigned long long g_idle_skipped_cycles = 0;
static unsigned long long g_idle_skipped_iters  = 0;
static unsigned long long g_idle_confirmed_sites = 0;

// P6 sljit differential gate — shadow-tick mode. While g_runtime_shadow_tick is
// set (during a throwaway validation or transactional guest re-run), runtime_tick
// accumulates the shard's cycle cost into g_runtime_shadow_cycles and does
// NOTHING else: no device pump, no IRQ delivery, no real-clock advance. MMIO
// catch-up/rescheduling obey the same contract below; otherwise merely reaching
// a trapped MMIO access could mutate devices before the transaction rejects it.
// Default 0 → normal play / the gcc path are untouched.
extern "C" unsigned          g_runtime_shadow_tick   = 0;
extern "C" unsigned long long g_runtime_shadow_cycles = 0;

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

static void dump_sample_hist(const char* tag) {
    std::vector<std::pair<uint32_t, uint64_t>> v(g_pc_hist.begin(),
                                                 g_pc_hist.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    uint64_t total = 0;
    for (const auto& p : v) total += p.second;
    if (total == 0) return;

    auto* ppu = g_active_ppu;
    std::fprintf(stderr,
                 "[sample] %s samples=%llu current_pc=0x%08X cpsr=0x%08X "
                 "cycles=%llu",
                 tag ? tag : "dump",
                 static_cast<unsigned long long>(total),
                 g_cpu.R[15],
                 g_cpu.cpsr,
                 static_cast<unsigned long long>(g_runtime_cycles));
    if (ppu) {
        std::fprintf(stderr, " ppu_frame=%llu vcount=%u",
                     static_cast<unsigned long long>(ppu->frame_count()),
                     static_cast<unsigned>(ppu->vcount()));
    }
    std::fprintf(stderr, "\n");

    for (std::size_t i = 0; i < v.size() && i < 30; ++i) {
        uint32_t off = 0;
        const char* sym = gba_symbol_lookup(v[i].first, &off);
        char symbuf[96];
        symbuf[0] = '\0';
        if (sym) {
            std::snprintf(symbuf, sizeof(symbuf), " <%s+0x%X>", sym, off);
        }
        std::fprintf(stderr, "  0x%08X%s  %5.2f%%  (%llu)\n",
                     v[i].first,
                     symbuf,
                     100.0 * static_cast<double>(v[i].second) /
                         static_cast<double>(total),
                     static_cast<unsigned long long>(v[i].second));
    }
    std::fflush(stderr);
}

static long long sampler_live_seconds() {
    const char* e = std::getenv("GBARECOMP_SAMPLE_LIVE_SECONDS");
    long long s = e ? std::atoll(e) : 0;
    return s > 0 ? s : 0;
}

static void sampler_loop() {
    const long long live_secs = sampler_live_seconds();
    auto next_dump = std::chrono::steady_clock::now() +
        std::chrono::seconds(live_secs > 0 ? live_secs : 86400);
    while (g_sampling.load(std::memory_order_relaxed)) {
        g_pc_hist[g_cpu.R[15]]++;  // racy read; approximate is fine
        if (live_secs > 0 && std::chrono::steady_clock::now() >= next_dump) {
            dump_sample_hist("live");
            next_dump = std::chrono::steady_clock::now() +
                std::chrono::seconds(live_secs);
        }
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
        dump_sample_hist("exit");
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

gba::GbaPpu* active_ppu() {
    return g_active_ppu;
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

// ── Stage 1: lazy device catch-up (MMIO access hooks) ───────────────────────
// runtime_tick now accumulates guest cycles and only materializes device state
// at the next scheduled-event horizon (see runtime_tick below). Any MMIO access
// must therefore first catch the devices up to 'now' so the access observes
// current state; a config-changing write additionally reschedules the horizon.
// Defined after tick_devices. The IO region is 0x04000000-0x040003FF (page 4).
extern "C" void runtime_mmio_catch_up(void);     // materialize lagged device state to now
extern "C" void runtime_resync_horizon(void);    // recompute next-event budget after a write
static inline bool is_io_addr(uint32_t addr) {
    return (addr & 0xFF000000u) == 0x04000000u;
}

// Stage 2: is a load from `addr` safe to treat as side-effect-free and stable
// inside an idle-candidate loop? Only writable RAM whose every mutation bumps
// g_idle_disturb_epoch (EWRAM/IWRAM) and immutable memory (ROM, BIOS) qualify.
// Everything else — MMIO (read side effects / FIFOs), the cartridge GPIO window
// (RTC etc. clock state out on read), save/flash (0x0E/0x0F, command state
// machine), palette/VRAM/OAM, open-bus/unmapped — is rejected: a load from such
// a region bumps the disturbance epoch so the loop can never reach a confirmed
// fixed point and is never elided. (ChatGPT-validated "probe memory-class
// check" — registers + epoch are sufficient ONLY with this guard. The bless of
// the omitted explicit load trace is conditional on it.)
static inline bool is_idle_safe_read(uint32_t addr) {
    const uint32_t region = addr >> 24;
    if (region == 0x02u || region == 0x03u) return true;   // EWRAM / IWRAM
    if (addr < 0x00004000u) return true;                   // BIOS (immutable)
    if (region >= 0x08u && region <= 0x0Du) {              // ROM (immutable) …
        const uint32_t off = addr & 0x01FFFFFFu;
        // … except the GPIO window 0x080000C4-0x080000C8 (RTC/sensor reads have
        // side effects). A small guard band covers wide accesses straddling it.
        if (off >= 0x000000C0u && off < 0x000000CCu) return false;
        return true;
    }
    return false;  // MMIO, palette, VRAM, OAM, save/flash, open-bus, unmapped
}

extern "C" uint32_t bus_read_u32(uint32_t addr) {
    if (is_io_addr(addr)) runtime_mmio_catch_up();
    if (!is_idle_safe_read(addr)) ++g_idle_disturb_epoch;
    gbarecomp::sync_bios_access();
    uint32_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read32(addr)
        : 0u;
    gbarecomp::trace_unmapped_read(addr, v, 4u);
    return v;
}

extern "C" uint16_t bus_read_u16(uint32_t addr) {
    if (is_io_addr(addr)) runtime_mmio_catch_up();
    if (!is_idle_safe_read(addr)) ++g_idle_disturb_epoch;
    gbarecomp::sync_bios_access();
    uint16_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read16(addr)
        : uint16_t{0};
    gbarecomp::trace_unmapped_read(addr, v, 2u);
    return v;
}

extern "C" uint8_t bus_read_u8(uint32_t addr) {
    if (is_io_addr(addr)) runtime_mmio_catch_up();
    if (!is_idle_safe_read(addr)) ++g_idle_disturb_epoch;
    gbarecomp::sync_bios_access();
    uint8_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read8(addr)
        : uint8_t{0};
    gbarecomp::trace_unmapped_read(addr, v, 1u);
    return v;
}

extern "C" void bus_write_u32(uint32_t addr, uint32_t val) {
    bool io = is_io_addr(addr);
    if (io) runtime_mmio_catch_up();
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write32(addr, val);
    ++g_idle_disturb_epoch;   // any guest write may change a watched poll value
    if (io) runtime_resync_horizon();
}

extern "C" void bus_write_u16(uint32_t addr, uint16_t val) {
    bool io = is_io_addr(addr);
    if (io) runtime_mmio_catch_up();
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write16(addr, val);
    ++g_idle_disturb_epoch;
    if (io) runtime_resync_horizon();
}

extern "C" void bus_write_u8(uint32_t addr, uint8_t val) {
    bool io = is_io_addr(addr);
    if (io) runtime_mmio_catch_up();
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write8(addr, val);
    ++g_idle_disturb_epoch;
    if (io) runtime_resync_horizon();
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
// True while inside tick_devices. Timed/FIFO DMA fires from here and accumulates
// its stolen cycles in GbaIo; the drain (which itself ticks devices) must run
// OUTSIDE this window to avoid re-entrancy — this guard makes that explicit.
static bool g_in_device_tick = false;
struct DeviceTickGuard {
    bool prev;
    DeviceTickGuard() : prev(g_in_device_tick) { g_in_device_tick = true; }
    ~DeviceTickGuard() { g_in_device_tick = prev; }
};

static void tick_devices(gba::GbaBus* bus, gba::GbaPpu* ppu, uint32_t cycles) {
    DeviceTickGuard _dtg;
    // Stage 2: materializing device state can raise IF, advance the PPU phase,
    // run timed DMA into watched RAM, etc. Any of these can change a polled
    // value or the timing rules, so it disturbs an in-flight idle proof.
    ++g_idle_disturb_epoch;
    uint32_t remaining = cycles;
    while (remaining != 0) {
        uint32_t chunk = remaining;
        uint32_t until_sample = bus->audio().cycles_until_next_sample();
        uint32_t until_timer = bus->io().cycles_until_next_timer_event();
        uint32_t until_sio = bus->io().cycles_until_next_sio_event();
        uint32_t until_ppu = ppu->cycles_until_next_event();
        if (until_sample < chunk) chunk = until_sample;
        if (until_timer < chunk) chunk = until_timer;
        if (until_sio < chunk) chunk = until_sio;
        if (until_ppu < chunk) chunk = until_ppu;
        if (chunk == 0) chunk = 1;

        bus->audio().tick(chunk);
        bus->io().tick_timers(chunk);
        bus->io().tick_sio(chunk);

        uint16_t vc_compare = static_cast<uint16_t>(
            (bus->io().dispstat() >> 8) & 0xFFu);
        auto events = ppu->tick(chunk, vc_compare);
        uint16_t ds = bus->io().dispstat();
        if (events.hblank_started &&
            ppu->vcount() < gba::GbaPpu::kLinesVisible) {
            std::chrono::steady_clock::time_point _prof_t0;
            if (g_phase_prof) _prof_t0 = std::chrono::steady_clock::now();
            ppu->render_scanline(ppu->vcount(),
                                 bus->io().read16(0x000),
                                 bus->io().raw(),
                                 bus->vram_ptr(),
                                 bus->oam_ptr(),
                                 bus->pal_ptr());
            if (g_phase_prof) {
                g_prof_scanline_ns += static_cast<unsigned long long>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - _prof_t0).count());
                ++g_prof_scanline_count;
            }
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

// ── LP-005 clock-event probe (env GBARECOMP_CYC_LO / GBARECOMP_CYC_HI) ───────
// One-shot diagnostic: when g_runtime_cycles is within [LO,HI], log every event
// that advances the master clock (runtime_tick, drain_dma_steal, resync) with
// the current guest PC. Off by default (both env unset). Harness-only; compiled
// out cost is a single env read cached in statics.
extern "C" const char* g_tick_ctx = "gen";  // set by interp/bridge paths around runtime_tick
static inline void cyc_probe(const char* what, uint32_t amt) {
    static long long lo = -1, hi = -1;
    if (lo < 0) {
        const char* l = std::getenv("GBARECOMP_CYC_LO");
        const char* h = std::getenv("GBARECOMP_CYC_HI");
        lo = l ? std::atoll(l) : 0;
        hi = h ? std::atoll(h) : 0;
    }
    if (hi == 0) return;  // probe disabled
    unsigned long long now = g_runtime_cycles;
    if (static_cast<long long>(now) < lo ||
        static_cast<long long>(now) > hi) return;
    std::fprintf(stderr, "[cycprobe] %-12s [%s] +%u  clk=%llu->%llu  pc=%08x cpsr=%08x\n",
                 what, g_tick_ctx, amt, now, now + amt, g_cpu.R[15], g_cpu.cpsr);
}

// ── Stage 1: lazy device catch-up state ─────────────────────────────────────
// g_pending_cycles: guest cycles advanced on the master clock but not yet
// materialized into device state. g_event_budget: cycles remaining until the
// next scheduled device event; when it reaches 0, runtime_tick materializes
// (tick_devices) and reschedules. tick_devices chunks internally to exact
// sub-event boundaries, so materializing a batched delta is bit-identical to
// per-instruction ticking — at a fraction of the call overhead.
static unsigned long long g_pending_cycles = 0;
static long long          g_event_budget   = 0;

static inline void recompute_event_budget(gba::GbaBus* bus, gba::GbaPpu* ppu) {
    uint32_t h  = ppu->cycles_until_next_event();
    uint32_t ut = bus->io().cycles_until_next_timer_event();
    uint32_t us = bus->audio().cycles_until_next_sample();
    uint32_t usio = bus->io().cycles_until_next_sio_event();
    if (ut < h) h = ut;
    if (us < h) h = us;
    if (usio < h) h = usio;
    if (h == 0u) h = 1u;
    g_event_budget = static_cast<long long>(h);
}

// Charge DMA-stolen bus cycles (accumulated by the GbaIo DMA loops) to the
// master clock and advance devices for that window — i.e. model cycle-stealing.
// Called OUTSIDE tick_devices (end of runtime_tick for timed/FIFO DMA; after a
// guest write for immediate DMA), so its own tick_devices is not re-entrant.
// Guarded against the (currently unreachable) in-tick case for safety.
static inline void drain_dma_steal(gba::GbaBus* bus, gba::GbaPpu* ppu) {
    if (!bus || !ppu) return;
    uint32_t cyc = bus->io().take_dma_steal_cycles();
    if (cyc == 0) return;
    cyc_probe("dma_steal", cyc);
    g_runtime_cycles += cyc;
    if (g_in_device_tick) {
        // Defensive: defer the device-advance into the pending window.
        g_pending_cycles += cyc;
        g_event_budget   -= static_cast<long long>(cyc);
        return;
    }
    if (g_pending_cycles) {
        tick_devices(bus, ppu, static_cast<uint32_t>(g_pending_cycles));
        g_pending_cycles = 0;
    }
    tick_devices(bus, ppu, cyc);
    recompute_event_budget(bus, ppu);
}

// Materialize lagged device state up to 'now'. Called before any MMIO access so
// the access observes current device state. Fires NO events: between flushes
// g_pending_cycles < g_event_budget (we flush the instant the budget hits 0),
// so the accumulated delta never reaches the next event boundary. The budget
// already accounts for these cycles (runtime_tick debited it as they accrued),
// so it is left unchanged here.
extern "C" void runtime_mmio_catch_up(void) {
    // A shadow/transactional re-run must not materialize the real machine's
    // pending time. The bus observer will reject or diagnose the MMIO access;
    // flushing here first would irreversibly advance PPU/audio/timers despite
    // the guest transaction subsequently being rolled back.
    if (g_runtime_shadow_tick) return;
    auto* bus = gbarecomp::g_active_bus;
    auto* ppu = gbarecomp::g_active_ppu;
    if (!bus || !ppu || g_pending_cycles == 0) return;
    tick_devices(bus, ppu, static_cast<uint32_t>(g_pending_cycles));
    g_pending_cycles = 0;
}

// Recompute the next-event horizon after a config-changing MMIO write (timer
// reload/control, DISPSTAT, DMA registers, etc. move the next event).
extern "C" void runtime_resync_horizon(void) {
    // Device stores are suppressed by the shadow transaction's bus observer.
    // Do not drain DMA or rewrite the real scheduler horizon for that rejected
    // store. This also keeps the established shadow-tick promise symmetric with
    // runtime_mmio_catch_up() and runtime_tick().
    if (g_runtime_shadow_tick) return;
    auto* bus = gbarecomp::g_active_bus;
    auto* ppu = gbarecomp::g_active_ppu;
    if (!bus || !ppu) return;
    // An immediate-mode DMA may have been triggered by the just-completed guest
    // write; charge its stolen cycles before re-arming the horizon.
    cyc_probe("resync", 0);
    drain_dma_steal(bus, ppu);
    recompute_event_budget(bus, ppu);
}

extern "C" void runtime_tick(uint32_t cycles) {
    // P6 shadow-tick: a healed shard's validation re-run only accumulates its
    // cycle cost (for the cycle diff); the interpreter pass already pumped the
    // devices / delivered IRQs for this window, so do nothing else here.
    if (g_runtime_shadow_tick) { g_runtime_shadow_cycles += cycles; return; }

    auto* bus = gbarecomp::g_active_bus;
    auto* ppu = gbarecomp::g_active_ppu;
    if (!bus || !ppu || cycles == 0) return;

    cyc_probe("tick", cycles);
    g_runtime_cycles += cycles;
    // Lazy device catch-up: advance the master clock every instruction (cheap),
    // but materialize device state only when the next-event horizon is reached.
    // (IRQ-eligibility is still checked every instruction below — that is cheap,
    // and IF can only change at a flush, so checking between flushes is exact.)
    g_pending_cycles += cycles;
    g_event_budget   -= static_cast<long long>(cycles);
    if (g_event_budget <= 0) {
        tick_devices(bus, ppu, static_cast<uint32_t>(g_pending_cycles));
        g_pending_cycles = 0;
        recompute_event_budget(bus, ppu);
    }

    // A timed/FIFO DMA may have fired inside that flush; charge its stolen bus
    // cycles (advancing the clock + devices) before the IRQ-eligibility check,
    // so a DMA-end IRQ or a timer that overflowed during the steal is seen now.
    drain_dma_steal(bus, ppu);

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
            // Materialize any cycles accumulated before the wake first (preserve
            // device ordering), then the wake-latency window, then re-arm the
            // horizon — all devices are now current as of this cycle.
            if (g_pending_cycles) {
                tick_devices(bus, ppu, static_cast<uint32_t>(g_pending_cycles));
                g_pending_cycles = 0;
            }
            cyc_probe("irq_wake", gba::kIrqWakeDelayCycles);
            g_runtime_cycles += gba::kIrqWakeDelayCycles;
            tick_devices(bus, ppu, gba::kIrqWakeDelayCycles);
            recompute_event_budget(bus, ppu);
        }
        runtime_irq(g_cpu.R[15]);
    }

#ifdef GBA_COSIM
    // First-divergence co-simulation checkpoint. runtime_tick is the shared
    // per-instruction master-clock advance for BOTH backends (generated code emits
    // it per instruction; the force-interp driver calls it per instruction), so
    // this is the alignment clock: it records/parks at cycle-stride boundaries so
    // the recomp and interp instances stop at identical guest cycles. Placed at the
    // very end so g_runtime_cycles reflects this instruction's cost + any DMA-steal
    // / HALT-wake cycles added above. See COSIM_ORACLE.md §3.
    cosim_on_tick();
#endif
}

// ── Stage 2: idle-loop elision prover ───────────────────────────────────────
// Per back-edge of a statically-eligible quiescent loop (codegen emits the
// call), prove the loop idle via a rolling fixed point and fast-forward the
// Stage-1 master clock to one period before the next scheduled event. See
// runtime_arm.h for the full contract and the cycle-accuracy argument.
namespace {
struct IdleSite {
    uint32_t regs[15];                 // R0-R14 at the previous back-edge
    uint32_t cpsr;                     // full CPSR at the previous back-edge
    unsigned long long last_cycles;    // g_runtime_cycles at previous back-edge
    unsigned long long last_epoch;     // g_idle_disturb_epoch at previous edge
    unsigned long long period;         // measured per-iteration cycle cost
    int  matches;                      // period-stable confirmations (>=1 → skip)
    bool have_period;
    bool valid;
};
static std::unordered_map<uint32_t, IdleSite> g_idle_sites;
static const bool g_idle_elision_on = [] {
    const char* e = std::getenv("GBARECOMP_IDLE_ELISION");
    bool on = !(e && e[0] == '0' && e[1] == '\0');   // default ON
    if (on) {
        std::atexit([] {
            if (g_idle_skipped_cycles == 0) return;
            std::fprintf(stderr,
                "[idle] elided %llu loop iterations (%llu cycles) over %llu "
                "skip ops across %zu site(s)\n",
                g_idle_skipped_iters, g_idle_skipped_cycles,
                g_idle_confirmed_sites, g_idle_sites.size());
        });
    }
    return on;
}();
}  // namespace

extern "C" void runtime_idle_backedge(uint32_t header_pc) {
    if (!g_idle_elision_on) return;
    if (g_runtime_shadow_tick) return;  // never alter time during a shadow re-run

    const unsigned long long now   = g_runtime_cycles;
    const unsigned long long epoch = g_idle_disturb_epoch;

    IdleSite& s = g_idle_sites[header_pc];

    bool regs_match = s.valid && s.cpsr == g_cpu.cpsr;
    if (regs_match) {
        for (int i = 0; i < 15; ++i) {
            if (s.regs[i] != g_cpu.R[i]) { regs_match = false; break; }
        }
    }
    const bool undisturbed = s.valid && (epoch == s.last_epoch);
    const unsigned long long delta = s.valid ? (now - s.last_cycles) : 0ull;

    // Rolling fixed point: the prior iteration left identical {R0-R14, CPSR},
    // nothing disturbed the watched state, and the period is stable. The loaded
    // poll value flows into a register, so an unchanged register set proves the
    // load returned the same value — no explicit load trace needed given the
    // static no-store / no-MMIO filter and the disturbance epoch.
    bool confirmed = false;
    if (regs_match && undisturbed && delta != 0ull) {
        if (s.have_period && delta == s.period) {
            if (++s.matches >= 1) confirmed = true;
        } else {
            s.period = delta;
            s.have_period = true;
            s.matches = 0;
        }
    } else {
        s.have_period = false;
        s.matches = 0;
    }

    if (confirmed && s.period != 0ull &&
        g_event_budget > static_cast<long long>(s.period)) {
        // next_event - now == g_event_budget (Stage-1 horizon). Skip whole
        // periods that all end strictly before it: periods = (budget-1)/period
        // guarantees now+skipped < next_event, so NO event fires in the hook.
        const unsigned long long budget =
            static_cast<unsigned long long>(g_event_budget);
        const unsigned long long periods = (budget - 1ull) / s.period;
        if (periods != 0ull) {
            const unsigned long long skipped = periods * s.period;
            g_runtime_cycles += skipped;   // master clock
            g_pending_cycles += skipped;   // materialized lazily at the horizon
            g_event_budget   -= static_cast<long long>(skipped);
            g_idle_skipped_cycles += skipped;
            g_idle_skipped_iters  += periods;
            ++g_idle_confirmed_sites;
        }
    }

    // Snapshot for the next iteration. Registers are unchanged by a skip;
    // last_cycles takes the post-skip clock so the next delta measures one real
    // period.
    for (int i = 0; i < 15; ++i) s.regs[i] = g_cpu.R[i];
    s.cpsr = g_cpu.cpsr;
    s.last_cycles = g_runtime_cycles;
    s.last_epoch  = g_idle_disturb_epoch;
    s.valid = true;
}

// Frame-present hook (present-in-place). When set, the per-VBlank frame-present
// yield does NOT unwind the guest stack: it calls this hook (which presents the
// frame, polls input, paces) and resumes the guest in place — so R15 is never
// re-dispatched from an arbitrary interior PC, eliminating the whole class of
// frame-boundary resume dispatch-misses. The hook returns true to request quit
// (then the yield DOES unwind, so the runner can exit). Set by the windowed
// runner only; unset for headless/TCP, which keep the unwind-and-redispatch path.
std::function<bool()> g_frame_present_hook;
// Sticky quit: once the present hook requests exit, EVERY subsequent yield must
// unwind (return true) so the guest's whole host call stack pops back to the
// runner — one return only frees one frame. Cleared when a hook is (re)set.
bool g_frame_present_quit = false;

void runtime_set_frame_present_hook(std::function<bool()> h) {
    g_frame_present_hook = std::move(h);
    g_frame_present_quit = false;
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

    // Present-in-place quit: fully unwind the guest to the runner once requested.
    if (g_frame_present_quit) return true;

    bool halted = bus && bus->io().halted();

    // Some games, including Pokemon FireRed, busy-wait on a VBlank flag instead
    // of entering HALT. Yield once per VBlank-start in normal guest modes so the
    // host runner can present frames, poll input, and honor --frames even while
    // the guest stays inside one long-running dispatch. Do NOT yield from inside
    // an exception handler; runtime_irq runs the whole IRQ chain synchronously
    // (runtime_dispatch(0x18) between ++/-- g_irq_nest_depth), so yielding there
    // would unwind that nested dispatch and abandon the handler before it acks
    // the interrupt — the FireRed VBlank freeze. The cpsr mode alone is NOT a
    // sufficient guard: a GBA IRQ dispatcher (intr_main) switches IRQ→System mode
    // mid-handler to allow nested IRQs, so it would pass a User/System-mode test
    // while still inside the IRQ. Gate on the live IRQ nesting depth too.
    static const bool yield_on_vblank = [] {
        const char* e = std::getenv("GBARECOMP_YIELD_ON_VBLANK");
        return !(e && e[0] == '0' && e[1] == '\0');
    }();
    if (yield_on_vblank && g_irq_nest_depth == 0u &&
        g_runtime_vblank_starts != g_runtime_yielded_vblank_start) {
        const uint32_t mode = g_cpu.cpsr & 0x1Fu;
        if (mode == 0x10u || mode == 0x1Fu) {
            g_runtime_yielded_vblank_start = g_runtime_vblank_starts;
            // Present-in-place when a hook is registered (windowed runner): the
            // frame is presented from here and the guest resumes WITHOUT
            // unwinding — no interior-PC re-dispatch, so no frame-boundary
            // dispatch-miss. Only unwind (return true) when the hook asks to
            // quit. With no hook (headless/TCP), keep the original unwind path.
            if (g_frame_present_hook) {
                bool quit = g_frame_present_hook();
                if (quit) g_frame_present_quit = true;  // sticky: full unwind
                return quit;
            }
            return true;
        }
    }

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
