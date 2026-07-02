// gba_io.cpp — see gba_io.h.

#include "gba_io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "gba_audio.h"
#include "gba_ppu.h"
#include "snapshot.h"

// Always-on MMIO write-trace ring (Axis 4). File-local statics mirroring the
// runtime_arm.cpp ring style; lazily allocated on the first write. The cycle and
// pc stamps come from the recomp runtime globals (declared here to avoid pulling
// the ArmCpuState layout into this lib).
extern "C" unsigned long long g_runtime_cycles;
extern "C" uint32_t runtime_current_pc(void);

namespace gba {
namespace {
MmioCapEntry* g_mmio_ring  = nullptr;
uint64_t      g_mmio_write = 0;      // total committed writes (monotonic)
bool          g_mmio_split = false;  // suppress nested write16 from write32

inline void mmio_cap_record(uint32_t addr, uint32_t value, uint32_t size) {
    if (!g_mmio_ring) {
        g_mmio_ring = static_cast<MmioCapEntry*>(
            std::calloc(kMmioCapRingSize, sizeof(MmioCapEntry)));
        if (!g_mmio_ring) return;
    }
    MmioCapEntry& e = g_mmio_ring[g_mmio_write % kMmioCapRingSize];
    e.cycle = g_runtime_cycles;
    e.addr  = addr;
    e.value = value;
    e.size  = size;
    e.pc    = runtime_current_pc();
    ++g_mmio_write;
}
}  // namespace

uint64_t gba_mmio_cap_total()  { return g_mmio_write; }
uint64_t gba_mmio_cap_oldest() {
    return g_mmio_write > kMmioCapRingSize ? g_mmio_write - kMmioCapRingSize : 0;
}
std::size_t gba_mmio_cap_query(uint64_t start, std::size_t count,
                               MmioCapEntry* out, uint64_t& out_first) {
    out_first = start;
    if (!g_mmio_ring || count == 0 || !out) return 0;
    uint64_t oldest = gba_mmio_cap_oldest();
    uint64_t head   = g_mmio_write;
    if (start < oldest) start = oldest;
    if (start >= head) { out_first = start; return 0; }
    uint64_t avail = head - start;
    if (count > avail) count = static_cast<std::size_t>(avail);
    out_first = start;
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = g_mmio_ring[(start + i) % kMmioCapRingSize];
    }
    return count;
}

void GbaIo::serialize(gbarecomp::debug::SnapshotWriter& w) const {
    w.bytes(io_.data(), io_.size());
    w.boolean(halted_);
    w.u64(unmapped_count_);
    for (int i = 0; i < 4; ++i) w.u64(dma_runs_[i]);
    for (int i = 0; i < 4; ++i) w.u64(dma_words_[i]);
    for (int i = 0; i < 4; ++i) w.u16(timer_reload_[i]);
    for (int i = 0; i < 4; ++i) w.u16(timer_counter_[i]);
    for (int i = 0; i < 4; ++i) w.u16(timer_control_[i]);
    for (int i = 0; i < 4; ++i) w.u32(timer_accum_[i]);
    for (int i = 0; i < 4; ++i) w.u32(dma_next_source_[i]);
    for (int i = 0; i < 4; ++i) w.u32(dma_next_dest_[i]);
}

uint64_t GbaIo::cosim_hash() const {
    // Guest-architectural IO only — EXCLUDES unmapped_count_ / dma_runs_ /
    // dma_words_ (emulator bookkeeping; see the header). Mirrors serialize()'s
    // field set minus those counters.
    uint64_t h = 1469598103934665603ull;
    auto f = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    f(io_.data(), io_.size());
    const uint8_t hb = halted_ ? 1u : 0u; f(&hb, 1);
    f(timer_reload_,     sizeof timer_reload_);
    f(timer_counter_,    sizeof timer_counter_);
    f(timer_control_,    sizeof timer_control_);
    f(timer_accum_,      sizeof timer_accum_);
    f(dma_next_source_,  sizeof dma_next_source_);
    f(dma_next_dest_,    sizeof dma_next_dest_);
    return h;
}

int GbaIo::cosim_dump(char* out, int cap) const {
    auto rd16 = [&](uint32_t off) -> unsigned {
        return static_cast<unsigned>(io_[off] | (io_[off + 1] << 8));
    };
    int n = std::snprintf(out, cap,
        "ie %04x if %04x ime %d dispstat %04x waitcnt %04x halted %d",
        ie(), if_reg(), ime() ? 1 : 0, dispstat(), rd16(0x204), halted_ ? 1 : 0);
    for (int i = 0; i < 4 && n < cap; ++i)
        n += std::snprintf(out + n, cap - n,
            " t%d_cnt %04x t%d_rld %04x t%d_ctl %04x t%d_acc %08x",
            i, timer_counter_[i], i, timer_reload_[i],
            i, timer_control_[i], i, timer_accum_[i]);
    for (int i = 0; i < 4 && n < cap; ++i)
        n += std::snprintf(out + n, cap - n, " d%d_src %08x d%d_dst %08x",
            i, dma_next_source_[i], i, dma_next_dest_[i]);
    if (n < cap) n += std::snprintf(out + n, cap - n, "\n");
    return n;
}

void GbaIo::deserialize(gbarecomp::debug::SnapshotReader& r) {
    r.bytes(io_.data(), io_.size());
    halted_         = r.boolean();
    unmapped_count_ = static_cast<std::size_t>(r.u64());
    for (int i = 0; i < 4; ++i) dma_runs_[i]        = static_cast<std::size_t>(r.u64());
    for (int i = 0; i < 4; ++i) dma_words_[i]       = static_cast<std::size_t>(r.u64());
    for (int i = 0; i < 4; ++i) timer_reload_[i]    = r.u16();
    for (int i = 0; i < 4; ++i) timer_counter_[i]   = r.u16();
    for (int i = 0; i < 4; ++i) timer_control_[i]   = r.u16();
    for (int i = 0; i < 4; ++i) timer_accum_[i]     = r.u32();
    for (int i = 0; i < 4; ++i) dma_next_source_[i] = r.u32();
    for (int i = 0; i < 4; ++i) dma_next_dest_[i]   = r.u32();
}

GbaIo::GbaIo() {
    // KEYINPUT default: all keys released. The register is active-low,
    // so 0x03FF (low 10 bits set) means "nothing pressed".
    io_[0x130] = 0xFF;
    io_[0x131] = 0x03;
    // SOUNDBIAS (0x088): default 0x0200 at power-on per GBATEK §
    // "GBA Sound Channel A, B (DMA Sound) — SOUNDBIAS". The BIOS
    // reads this value during init (it computes audio offsets from
    // it); leaving it as 0 made our run drift from real hardware at
    // BIOS instruction #743 (LDRH from 0x04000088).
    io_[0x088] = 0x00;
    io_[0x089] = 0x02;
}
GbaIo::~GbaIo() = default;

namespace {

// LE byte access on the backing array.
uint16_t load_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t load_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) |
                                 (p[2] << 16) | (p[3] << 24));
}
void store_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void store_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
// Once-per-key warning set so the BIOS setup phase doesn't drown
// stderr in identical messages.
std::unordered_set<uint64_t>& warned_set() {
    static std::unordered_set<uint64_t> s;
    return s;
}

}  // namespace

// GBATEK § "GBA DMA Transfers" timing: transferring `units` data units costs
// read 1N+(units-1)S + write 1N+(units-1)S bus cycles, plus a ~2-cycle startup.
// N/S come from the source/dest region wait-states (GbaBus::access_cycles). The
// region is taken from the start addresses (DMA blocks stay within one region in
// practice; FIFO dest is fixed in IO). This is the cycle the CPU is stalled.
uint32_t GbaIo::dma_transfer_cost(uint32_t src, uint32_t dst, bool transfer_32,
                                  uint32_t units) const {
    if (!bus_ || units == 0) return 0;
    uint8_t w = transfer_32 ? 4u : 2u;
    uint32_t rN = bus_->access_cycles(src, w, false);
    uint32_t rS = bus_->access_cycles(src, w, true);
    uint32_t wN = bus_->access_cycles(dst, w, false);
    uint32_t wS = bus_->access_cycles(dst, w, true);
    uint32_t read  = rN + (units - 1u) * rS;
    uint32_t write = wN + (units - 1u) * wS;
    return read + write + 2u;
}

// Run a DMA channel in immediate mode.
//
// GBATEK § "GBA DMA Transfers":
//   DMAxSAD  — source address (27 bits for DMA0; 28 bits for 1..3)
//   DMAxDAD  — dest address   (27 bits for 0/1/2; 28 bits for DMA3)
//   DMAxCNT_L— word count (16 bits; 0 = 0x10000 for DMA3, 0x4000 for others)
//   DMAxCNT_H bits:
//     5..6   dest control (0=inc, 1=dec, 2=fixed, 3=inc+reload)
//     7..8   src control  (0=inc, 1=dec, 2=fixed)
//     9      repeat
//     10     32-bit transfer (else 16-bit)
//     11     gamepak DRQ (DMA3 only)
//     12..13 start timing (0=immediate)
//     14     IRQ on end
//     15     enable
//
// Phase 2.5 scope: immediate-mode (start_mode=0), no-repeat. Other
// timings (VBlank, HBlank, sound FIFO, video) land in 2.6+.
void GbaIo::run_immediate_dma(int channel) {
    if (!bus_) return;
    uint32_t base = 0xB0u + static_cast<uint32_t>(channel) * 12u;
    uint32_t sad = load_u32(&io_[base + 0]) & 0x0FFFFFFFu;
    uint32_t dad = load_u32(&io_[base + 4]) & 0x0FFFFFFFu;
    uint32_t cnt_l = load_u16(&io_[base + 8]);
    uint16_t cnt_h = load_u16(&io_[base + 10]);

    // Word count: 0 means max for the channel.
    uint32_t word_count = cnt_l;
    if (word_count == 0) {
        word_count = (channel == 3) ? 0x10000u : 0x4000u;
    }

    bool transfer_32 = (cnt_h & 0x0400u) != 0;
    uint32_t step = transfer_32 ? 4u : 2u;

    // dest_ctrl: 0=inc, 1=dec, 2=fixed, 3=inc+reload
    uint32_t dest_ctrl = (cnt_h >> 5) & 0x3u;
    // src_ctrl:  0=inc, 1=dec, 2=fixed (3 prohibited)
    uint32_t src_ctrl  = (cnt_h >> 7) & 0x3u;

    auto step_addr = [&](uint32_t& addr, uint32_t ctrl) {
        switch (ctrl) {
            case 0: addr += step; break;
            case 1: addr -= step; break;
            case 2: break;        // fixed
            case 3: addr += step; break;  // inc+reload acts like inc during xfer
        }
    };

    ++dma_runs_[channel];
    dma_words_[channel] += word_count;

    // Always-on DMA-write watchpoint (MC-HP-002). DMA writes go straight through
    // bus_->write*, bypassing the recompiled-code runtime_trace path, so the CPU
    // mem-write watchpoint can't see them. When GBARECOMP_DMA_WATCH_ADDR falls in
    // a transfer's dest range, report the channel/source/dest/value so a corrupt
    // value landing in RAM via DMA (e.g. an animation frame-pointer) is
    // attributable. Pure observation. -1 = disabled.
    static long long dma_watch = -2;
    if (dma_watch == -2) {
        const char* e = std::getenv("GBARECOMP_DMA_WATCH_ADDR");
        dma_watch = e ? static_cast<long long>(std::strtoull(e, nullptr, 0)) : -1;
    }

    uint32_t s = sad;
    uint32_t d = dad;
    for (uint32_t k = 0; k < word_count; ++k) {
        if (transfer_32) {
            bus_->write32(d, bus_->read32(s));
        } else {
            bus_->write16(d, bus_->read16(s));
        }
        if (dma_watch >= 0 && d == static_cast<uint32_t>(dma_watch)) {
            uint32_t v = transfer_32 ? bus_->read32(d) : bus_->read16(d);
            std::fprintf(stderr,
                "[dma-watch] ch=%d dad=0x%08X (hit 0x%08X) src=0x%08X val=0x%08X "
                "size=%u word=%u/%u cnt_h=0x%04X\n",
                channel, dad, d, s, v, step, k, word_count, cnt_h);
        }
        step_addr(s, src_ctrl);
        step_addr(d, dest_ctrl);
    }

    // DMA stole this many bus cycles from the CPU.
    dma_steal_cycles_ += dma_transfer_cost(sad, dad, transfer_32, word_count);

    // Clear enable bit unless repeat is set (we don't model repeat
    // yet — for immediate mode it should be 0 in real ROMs anyway).
    if ((cnt_h & 0x0200u) == 0) {
        store_u16(&io_[base + 10], static_cast<uint16_t>(cnt_h & ~0x8000u));
    }

    // IRQ on completion (DMA channel bits 8..11 in IF).
    if (cnt_h & 0x4000u) {
        request_irq(static_cast<uint16_t>(1u << (8 + channel)));
    }
}

// VBlank/HBlank-timed DMA. The CNT_H enable + running SAD/DAD were latched on
// the rising enable edge (write16); here we actually transfer, once per trigger
// (per VBlank-start for mode 1, per visible-line HBlank-start for mode 2). The
// running SAD/DAD (dma_next_source_/dest_) carry across triggers, so an HBlank
// channel walks a per-scanline source table — e.g. the WIN0H circle table that
// draws a transition iris (MC-HP-003). Sound-FIFO mode 3 is handled separately
// (run_sound_fifo_dma); DMA3 video-capture mode 3 is not modeled.
void GbaIo::run_timed_dma(int start_mode) {
    if (!bus_) return;
    static long long dma_watch = -2;
    if (dma_watch == -2) {
        const char* e = std::getenv("GBARECOMP_DMA_WATCH_ADDR");
        dma_watch = e ? static_cast<long long>(std::strtoull(e, nullptr, 0)) : -1;
    }
    for (int channel = 0; channel < 4; ++channel) {
        uint32_t base  = 0xB0u + static_cast<uint32_t>(channel) * 12u;
        uint16_t cnt_h = load_u16(&io_[base + 10]);
        if ((cnt_h & 0x8000u) == 0) continue;                 // not enabled
        if (static_cast<int>((cnt_h >> 12) & 0x3u) != start_mode) continue;

        uint32_t cnt_l = load_u16(&io_[base + 8]);
        uint32_t word_count = cnt_l ? cnt_l : ((channel == 3) ? 0x10000u : 0x4000u);
        bool transfer_32   = (cnt_h & 0x0400u) != 0;
        uint32_t step      = transfer_32 ? 4u : 2u;
        uint32_t dest_ctrl = (cnt_h >> 5) & 0x3u;   // 0 inc,1 dec,2 fixed,3 inc+reload
        uint32_t src_ctrl  = (cnt_h >> 7) & 0x3u;   // 0 inc,1 dec,2 fixed

        uint32_t s = dma_next_source_[channel];
        uint32_t d = dma_next_dest_[channel];
        dma_steal_cycles_ += dma_transfer_cost(s, d, transfer_32, word_count);
        for (uint32_t k = 0; k < word_count; ++k) {
            if (transfer_32) bus_->write32(d, bus_->read32(s));
            else             bus_->write16(d, bus_->read16(s));
            if (dma_watch >= 0 && d == static_cast<uint32_t>(dma_watch)) {
                uint32_t v = transfer_32 ? bus_->read32(d) : bus_->read16(d);
                std::fprintf(stderr,
                    "[dma-watch:timed] mode=%d ch=%d dad=0x%08X src=0x%08X "
                    "val=0x%08X size=%u word=%u/%u cnt_h=0x%04X\n",
                    start_mode, channel, d, s, v, step, k, word_count, cnt_h);
            }
            if (src_ctrl == 0) s += step; else if (src_ctrl == 1) s -= step;
            if (dest_ctrl == 0 || dest_ctrl == 3) d += step;
            else if (dest_ctrl == 1) d -= step;
        }
        ++dma_runs_[channel];
        dma_words_[channel] += word_count;
        dma_next_source_[channel] = s;

        const bool repeat = (cnt_h & 0x0200u) != 0;
        if (repeat) {
            // Re-arm for the next trigger. dest_ctrl==3 (inc+reload) reloads DAD
            // from the register each repeat; otherwise the running DAD carries on.
            dma_next_dest_[channel] = (dest_ctrl == 3)
                ? (load_u32(&io_[base + 4]) & 0x0FFFFFFFu) : d;
        } else {
            dma_next_dest_[channel] = d;
            store_u16(&io_[base + 10], static_cast<uint16_t>(cnt_h & ~0x8000u));
        }
        if (cnt_h & 0x4000u) {
            request_irq(static_cast<uint16_t>(1u << (8 + channel)));
        }
    }
}

void GbaIo::run_sound_fifo_dma(int channel) {
    if (!bus_ || channel < 1 || channel > 2) return;
    uint32_t base = 0xB0u + static_cast<uint32_t>(channel) * 12u;
    uint16_t cnt_h = load_u16(&io_[base + 10]);
    if ((cnt_h & 0x8000u) == 0) return;
    if (((cnt_h >> 12) & 0x3u) != 3) return;
    if ((cnt_h & 0x0400u) == 0) return;

    uint32_t sad = dma_next_source_[channel] ?
        dma_next_source_[channel] : (load_u32(&io_[base + 0]) & 0x0FFFFFFFu);
    uint32_t dad = dma_next_dest_[channel] ?
        dma_next_dest_[channel] : load_u32(&io_[base + 4]);
    uint32_t src_ctrl = (cnt_h >> 7) & 0x3u;

    ++dma_runs_[channel];
    dma_words_[channel] += 4;

    // Sound-FIFO DMA is always 4 units of 32-bit; dest (FIFO) is fixed.
    dma_steal_cycles_ += dma_transfer_cost(sad, dad, /*transfer_32=*/true, 4);
    uint32_t s = sad;
    for (int i = 0; i < 4; ++i) {
        bus_->write32(dad, bus_->read32(s));
        switch (src_ctrl) {
            case 0: s += 4; break;
            case 1: s -= 4; break;
            case 2: break;
            default: s += 4; break;
        }
    }
    dma_next_source_[channel] = s;
    dma_next_dest_[channel] = dad;
}

void GbaIo::tick_timers(uint32_t cycles) {
    static constexpr uint32_t kPrescale[4] = {1, 64, 256, 1024};
    for (int t = 0; t < 4; ++t) {
        uint16_t ctrl = timer_control_[t];
        if ((ctrl & 0x0080u) == 0) continue;
        if (ctrl & 0x0004u) continue;  // cascade timers are not needed by BIOS audio

        uint32_t prescale = kPrescale[ctrl & 0x3u];
        timer_accum_[t] += cycles;
        uint32_t ticks = timer_accum_[t] / prescale;
        timer_accum_[t] %= prescale;
        if (ticks == 0) continue;

        while (ticks != 0) {
            uint32_t until_overflow = 0x10000u - timer_counter_[t];
            if (ticks < until_overflow) {
                timer_counter_[t] = static_cast<uint16_t>(
                    timer_counter_[t] + ticks);
                ticks = 0;
                break;
            }

            ticks -= until_overflow;
            timer_counter_[t] = timer_reload_[t];
            if (ctrl & 0x0040u) {
                request_irq(static_cast<uint16_t>(IrqTimer0 << t));
            }
            if (audio_ && (t == 0 || t == 1)) {
                uint8_t snd_h = io_[0x083];
                int fifo_a_timer = (snd_h & 0x04u) ? 1 : 0;
                int fifo_b_timer = (snd_h & 0x40u) ? 1 : 0;
                bool refill_a = t == fifo_a_timer && audio_->fifo_needs_dma(0);
                bool refill_b = t == fifo_b_timer && audio_->fifo_needs_dma(1);
                audio_->timer_overflow(t);
                if (refill_a) {
                    run_sound_fifo_dma(1);
                }
                if (refill_b) {
                    run_sound_fifo_dma(2);
                }
            }
        }

        store_u16(&io_[0x100u + static_cast<uint32_t>(t) * 4u],
                  timer_counter_[t]);
    }
}

uint32_t GbaIo::cycles_until_next_timer_event() const {
    static constexpr uint32_t kPrescale[4] = {1, 64, 256, 1024};
    uint32_t best = 0xFFFFFFFFu;
    for (int t = 0; t < 4; ++t) {
        uint16_t ctrl = timer_control_[t];
        if ((ctrl & 0x0080u) == 0) continue;
        if (ctrl & 0x0004u) continue;
        uint32_t prescale = kPrescale[ctrl & 0x3u];
        uint32_t ticks = 0x10000u - timer_counter_[t];
        uint64_t cycles = static_cast<uint64_t>(ticks) * prescale;
        if (cycles > timer_accum_[t]) cycles -= timer_accum_[t];
        else cycles = 1;
        if (cycles < best) best = static_cast<uint32_t>(cycles);
    }
    return best == 0xFFFFFFFFu ? 0xFFFFFFFFu : (best ? best : 1u);
}

void GbaIo::request_irq(uint16_t bit) {
    uint16_t old = load_u16(&io_[IoReg::IF]);
    store_u16(&io_[IoReg::IF], static_cast<uint16_t>(old | bit));
}

void GbaIo::tick_sio(uint32_t cycles) {
    if (!sio_transfer_active_) return;
    if (cycles < sio_cycles_remaining_) {
        sio_cycles_remaining_ -= cycles;
        return;
    }
    // Transfer complete.
    sio_transfer_active_  = false;
    sio_cycles_remaining_ = 0;
    // Auto-clear the start/busy bit.
    uint16_t cnt = load_u16(&io_[IoReg::SIOCNT]);
    store_u16(&io_[IoReg::SIOCNT], static_cast<uint16_t>(cnt & ~0x0080u));
    // With no connected partner the shift register reads back all-ones.
    store_u32(&io_[IoReg::SIODATA32], 0xFFFFFFFFu);
    // Serial IRQ on completion if enabled (SIOCNT bit 14).
    if (cnt & 0x4000u) request_irq(IrqSerial);
}

uint32_t GbaIo::cycles_until_next_sio_event() const {
    if (!sio_transfer_active_) return 0xFFFFFFFFu;
    return sio_cycles_remaining_ ? sio_cycles_remaining_ : 1u;
}

void GbaIo::set_keyinput(uint16_t keys) {
    store_u16(&io_[IoReg::KEYINPUT], static_cast<uint16_t>(keys & 0x03FFu));
}

bool GbaIo::irq_pending() const {
    uint16_t ie_v = load_u16(&io_[IoReg::IE]);
    uint16_t if_v = load_u16(&io_[IoReg::IF]);
    if ((ie_v & if_v) == 0) return false;
    // IME is a 16-bit register; only bit 0 is meaningful.
    return (load_u16(&io_[IoReg::IME]) & 0x0001u) != 0;
}

uint16_t GbaIo::ie()      const { return load_u16(&io_[IoReg::IE]); }
uint16_t GbaIo::if_reg()  const { return load_u16(&io_[IoReg::IF]); }
bool     GbaIo::ime()     const { return (load_u16(&io_[IoReg::IME]) & 1u) != 0; }
uint16_t GbaIo::dispstat() const { return load_u16(&io_[IoReg::DISPSTAT]); }

void GbaIo::warn_unhandled(uint32_t off, uint32_t value, bool is_write, uint8_t width) {
    ++unmapped_count_;
    uint64_t key = (static_cast<uint64_t>(off) << 16) |
                   (static_cast<uint64_t>(width) << 8) |
                   (is_write ? 1u : 0u);
    if (warned_set().insert(key).second) {
        std::fprintf(stderr,
                     "[gba:io] unhandled %s%u @ 0x%08x = 0x%x\n",
                     is_write ? "W" : "R", width,
                     0x04000000u + off, value);
    }
}

// ─────────────────────────────────────────────────────────────────────
// Reads
// ─────────────────────────────────────────────────────────────────────

uint8_t GbaIo::read8(uint32_t off) {
    if (off >= kIoSize) { warn_unhandled(off, 0, false, 1); return 0; }
    // POSTFLG: low bit is the boot flag. We're always "first boot"
    // for now, so return 0.
    if (off == IoReg::POSTFLG) return io_[IoReg::POSTFLG] & 0x01u;
    // VCOUNT is a 16-bit register at 0x006; byte access reads its
    // low byte (which IS the scanline value, since VCOUNT only
    // populates bits 0..7).
    if (off == IoReg::VCOUNT && ppu_) {
        return static_cast<uint8_t>(ppu_->vcount() & 0xFF);
    }
    // DISPSTAT low byte exposes the VBlank / HBlank / VCount-match
    // flags in bits 0..2. The BIOS reads DISPSTAT as a byte too.
    if (off == IoReg::DISPSTAT && ppu_) {
        uint8_t base = io_[IoReg::DISPSTAT];
        uint8_t flags = 0;
        if (ppu_->in_vblank()) flags |= 0x01u;
        if (ppu_->in_hblank()) flags |= 0x02u;
        // VCount-match: bit 2 set when VCOUNT == DISPSTAT[15:8].
        uint8_t target = io_[IoReg::DISPSTAT + 1];
        if (ppu_->vcount() == target) flags |= 0x04u;
        return (base & 0xF8u) | flags;
    }
    return io_[off];
}

uint16_t GbaIo::read16(uint32_t off) {
    if (off + 1 >= kIoSize) { warn_unhandled(off, 0, false, 2); return 0; }
    if (off == IoReg::VCOUNT) {
        return ppu_ ? ppu_->vcount() : 0;
    }
    if (off == IoReg::DISPSTAT && ppu_) {
        uint16_t base = load_u16(&io_[IoReg::DISPSTAT]);
        uint16_t flags = 0;
        if (ppu_->in_vblank()) flags |= 0x0001u;
        if (ppu_->in_hblank()) flags |= 0x0002u;
        uint16_t target = (base >> 8) & 0xFFu;
        if (ppu_->vcount() == target) flags |= 0x0004u;
        return static_cast<uint16_t>((base & 0xFFF8u) | flags);
    }
    if (off >= 0x100u && off <= 0x10Cu && ((off - 0x100u) % 4u) == 0) {
        int timer = static_cast<int>((off - 0x100u) / 4u);
        return timer_counter_[timer];
    }
    return load_u16(&io_[off]);
}

uint32_t GbaIo::read32(uint32_t off) {
    if (off + 3 >= kIoSize) { warn_unhandled(off, 0, false, 4); return 0; }
    // 32-bit IO reads are routed as two 16-bit reads so dynamic
    // registers (DISPSTAT, VCOUNT, timers, etc.) expose their live
    // values instead of stale backing-array bytes.
    uint32_t lo = read16(off);
    uint32_t hi = read16(off + 2);
    return lo | (hi << 16);
}

// ─────────────────────────────────────────────────────────────────────
// Writes
// ─────────────────────────────────────────────────────────────────────

void GbaIo::write8(uint32_t off, uint8_t v) {
    if (off >= kIoSize) { warn_unhandled(off, v, true, 1); return; }
    mmio_cap_record(0x04000000u + off, v, 1);
    // Audio registers (0x060..0x0AF) go to the audio subsystem on
    // top of the backing-array store, so the channel state machines
    // see triggers / envelope reloads / length resets.
    if (audio_ && off >= 0x060 && off <= 0x0AF) {
        audio_->write_io8(off, v);
    }
    switch (off) {
        case IoReg::HALTCNT:
            // 0x00 → HALT (CPU stops until next IRQ).
            // 0x80 → STOP (deeper sleep; we treat the same for now).
            halted_ = true;
            io_[off] = v;
            return;
        case IoReg::POSTFLG:
            // Cold-boot path is "post = 0"; warm-boot sets to 1.
            // Persist the byte; the BIOS uses it to detect re-entry.
            io_[off] = v & 0x01u;
            return;
        case IoReg::KEYINPUT:
        case IoReg::KEYINPUT + 1:
            // Read-only keypad state. BIOS init writes across broad
            // IO ranges; those writes must not make "all keys pressed."
            return;
        case IoReg::IF:        // 0x202: low byte of IF
        case IoReg::IF + 1: {  // 0x203: high byte of IF
            // Byte-write to IF: write-1-to-clear within that byte
            // only. The BIOS IRQ handler uses `STRB R1, [R3, #0x202]`
            // to acknowledge — without this branch the IF bit never
            // clears and the CPU re-enters the IRQ handler forever.
            io_[off] = static_cast<uint8_t>(io_[off] & ~v);
            return;
        }
        default:
            break;
    }
    io_[off] = v;
}

void GbaIo::write16(uint32_t off, uint16_t v) {
    if (off + 1 >= kIoSize) { warn_unhandled(off, v, true, 2); return; }
    if (!g_mmio_split) mmio_cap_record(0x04000000u + off, v, 2);
    if (audio_ && off >= 0x060 && off <= 0x0AF) {
        audio_->write_io16(off, v);
    }
    switch (off) {
        case IoReg::VCOUNT:
            // Read-only on hardware.
            return;
        case IoReg::KEYINPUT:
            return;
        case IoReg::DISPSTAT: {
            // Bits 0..2 (VBlank/HBlank/VCount) are read-only; bits
            // 3..15 (VBlank-IRQ / HBlank-IRQ / VCount-IRQ / VCount
            // setting) are writable.
            uint16_t old = load_u16(&io_[off]);
            uint16_t merged = (old & 0x0007) | (v & 0xFFF8);
            store_u16(&io_[off], merged);
            return;
        }
        case IoReg::IF: {
            // Write-1-to-clear: any bit set in `v` clears the
            // corresponding pending interrupt.
            uint16_t old = load_u16(&io_[off]);
            store_u16(&io_[off], static_cast<uint16_t>(old & ~v));
            return;
        }
        case 0x082:
            // SOUNDCNT_H reset bits (FIFO A/B clear) are write-only.
            store_u16(&io_[off], static_cast<uint16_t>(v & ~0x8800u));
            return;
        case IoReg::SIOCNT: {
            // SIO control. In Normal mode with the internal shift clock
            // (bit 0 = 1), writing the start/busy bit (bit 7) kicks a
            // transfer; on completion the bit auto-clears and — if bit 14
            // (IRQ enable) is set — the Serial IRQ fires. Games (e.g. the
            // Minish Cap) re-arm it from the handler to get a periodic IRQ.
            // External-clock (slave) transfers never complete without a
            // partner, so we don't arm those. (GBATEK § "SIO Normal Mode".)
            uint16_t old = load_u16(&io_[off]);
            store_u16(&io_[off], v);
            bool start_edge    = (old & 0x0080u) == 0 && (v & 0x0080u) != 0;
            bool internal_clk  = (v & 0x0001u) != 0;
            if (start_edge && internal_clk && !sio_transfer_active_) {
                // Transfer duration: bit 1 = 2 MHz(1)/256 KHz(0) clock,
                // bit 12 = 32-bit(1)/8-bit(0). Cycle table matches GBATEK /
                // the cycle-accurate reference: {256K·8b, 2M·8b, 256K·32b,
                // 2M·32b} = {512, 64, 2048, 256}.
                static constexpr uint32_t kSioCycles[4] = {512u, 64u, 2048u,
                                                           256u};
                uint32_t idx = ((v >> 1) & 1u) | ((v >> 11) & 2u);
                sio_cycles_remaining_ = kSioCycles[idx];
                sio_transfer_active_  = true;
            }
            return;
        }
        default:
            break;
    }
    if (off >= 0x100u && off <= 0x10Eu) {
        uint32_t rel = off - 0x100u;
        int timer = static_cast<int>(rel / 4u);
        if (timer >= 0 && timer < 4 && (rel % 4u) == 0) {
            timer_reload_[timer] = v;
            store_u16(&io_[off], v);
            return;
        }
        if (timer >= 0 && timer < 4 && (rel % 4u) == 2) {
            uint16_t old = timer_control_[timer];
            timer_control_[timer] = v;
            store_u16(&io_[off], v);
            if ((old & 0x0080u) == 0 && (v & 0x0080u) != 0) {
                timer_counter_[timer] = timer_reload_[timer];
                timer_accum_[timer] = 0;
                store_u16(&io_[0x100u + static_cast<uint32_t>(timer) * 4u],
                          timer_counter_[timer]);
            }
            return;
        }
    }
    // DMA control-high writes: storing CNT_H with the enable bit set
    // and start_mode=0 (immediate) triggers a DMA transfer.
    //
    // DMA channels live at IO offsets 0xBA (DMA0 CNT_H), 0xC6, 0xD2,
    // 0xDE — each channel block is 12 bytes (SAD 4, DAD 4, CNT_L 2,
    // CNT_H 2), starting at 0xB0/0xBC/0xC8/0xD4.
    for (int ch = 0; ch < 4; ++ch) {
        uint32_t cnt_h_off = 0xB0u + ch * 12u + 10u;
        if (off == cnt_h_off) {
            uint16_t old_h = load_u16(&io_[off]);
            store_u16(&io_[off], v);
            // Trigger only on rising edge of enable AND immediate mode.
            bool was_enabled = (old_h & 0x8000u) != 0;
            bool now_enabled = (v & 0x8000u) != 0;
            uint32_t start_mode = (v >> 12) & 0x3u;
            if (!was_enabled && now_enabled) {
                dma_next_source_[ch] = load_u32(&io_[0xB0u + ch * 12u]) &
                    0x0FFFFFFFu;
                dma_next_dest_[ch] = load_u32(&io_[0xB0u + ch * 12u + 4u]);
            }
            if (!was_enabled && now_enabled && start_mode == 0) {
                run_immediate_dma(ch);
            }
            return;
        }
    }
    store_u16(&io_[off], v);
}

void GbaIo::write32(uint32_t off, uint32_t v) {
    if (off + 3 >= kIoSize) { warn_unhandled(off, v, true, 4); return; }
    mmio_cap_record(0x04000000u + off, v, 4);
    if (audio_ && off >= 0x060 && off <= 0x0AF) {
        audio_->write_io32(off, v);
        if (off == 0x080) {
            store_u16(&io_[0x080], static_cast<uint16_t>(v & 0xFFFFu));
            store_u16(&io_[0x082], static_cast<uint16_t>((v >> 16) & ~0x8800u));
        } else {
            store_u32(&io_[off], v);
        }
        return;
    }
    // 32-bit IO writes are routed as two 16-bit writes so the
    // per-register quirks (DISPSTAT mask, IF write-1-to-clear, etc.)
    // apply consistently. Suppress the nested write16 MMIO-ring taps so the
    // 32-bit access is recorded as ONE size-4 entry (peer to the oracle), not
    // two size-2 sub-writes — the split is purely our internal decomposition.
    g_mmio_split = true;
    write16(off, static_cast<uint16_t>(v & 0xFFFF));
    write16(off + 2, static_cast<uint16_t>((v >> 16) & 0xFFFF));
    g_mmio_split = false;
}

}  // namespace gba
