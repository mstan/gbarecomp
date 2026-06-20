// gba_audio.cpp — see gba_audio.h. SOUND1/2 (square), SOUND3 (wave RAM),
// SOUND4 (noise), and Direct Sound A/B FIFOs, mixed in mix_one_sample().

#include "gba_audio.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "snapshot.h"

namespace gba {

namespace {

// Duty patterns for the four duty cycles (12.5 / 25 / 50 / 75 %).
// 8-step waveform; each entry is "1 if output high at this phase".
constexpr uint8_t kDutyPatterns[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},  // 12.5%
    {1, 0, 0, 0, 0, 0, 0, 1},  // 25%
    {1, 0, 0, 0, 0, 1, 1, 1},  // 50%
    {0, 1, 1, 1, 1, 1, 1, 0},  // 75% (alias of 25% inverted)
};

constexpr std::size_t kRingSize = 1u << 14;  // 16384 samples ~ 0.5 s

}  // namespace

GbaAudio::GbaAudio() {
    ring_.assign(kRingSize, 0);
    reset();
}
GbaAudio::~GbaAudio() = default;

void GbaAudio::serialize(gbarecomp::debug::SnapshotWriter& w) const {
    // Channels + FIFOs are POD nested structs (no pointers); raw bytes
    // are stable within a build and the snapshot version gates layout.
    w.bytes(&ch1_, sizeof(ch1_));
    w.bytes(&ch2_, sizeof(ch2_));
    // NOTE: ch3_/ch4_/wave_ram_ are intentionally NOT serialized — adding them
    // would change the blob layout and savestates are version-locked
    // (snapshot.h kSnapshotVersion), invalidating every existing state file.
    // Their state is transient (the MP2K driver re-triggers the PSG channels
    // each frame and rewrites wave RAM per note), so a save/load boundary
    // re-syncs them within ~1 frame. Revisit if/when kSnapshotVersion bumps.
    w.boolean(master_enable_);
    w.u8(volume_l_);
    w.u8(volume_r_);
    w.boolean(ch1_left_enable_);
    w.boolean(ch1_right_enable_);
    w.boolean(ch2_left_enable_);
    w.boolean(ch2_right_enable_);
    w.u8(dmg_volume_ratio_);
    w.bytes(&fifo_a_, sizeof(fifo_a_));
    w.bytes(&fifo_b_, sizeof(fifo_b_));
    w.boolean(direct_a_left_);
    w.boolean(direct_a_right_);
    w.boolean(direct_a_timer1_);
    w.boolean(direct_a_full_volume_);
    w.boolean(direct_b_left_);
    w.boolean(direct_b_right_);
    w.boolean(direct_b_timer1_);
    w.boolean(direct_b_full_volume_);
    w.u16(soundbias_);
    w.u32(cycles_per_sample_);
    w.u32(cycle_accumulator_);
    // Pending host output ring so playback resumes seamlessly.
    w.u32(static_cast<uint32_t>(ring_.size()));
    if (!ring_.empty()) w.bytes(ring_.data(), ring_.size() * sizeof(int16_t));
    w.u64(ring_head_);
    w.u64(ring_tail_);
    w.u64(samples_generated_);
    w.bytes(current_samples_, sizeof(current_samples_));
    w.u32(sample_index_);
}

void GbaAudio::deserialize(gbarecomp::debug::SnapshotReader& r) {
    r.bytes(&ch1_, sizeof(ch1_));
    r.bytes(&ch2_, sizeof(ch2_));
    // ch3_/ch4_/wave_ram_ not serialized — see serialize(). Reset them so a
    // loaded state starts the new channels clean (they re-sync within ~1 frame).
    ch3_ = Sound3{};
    ch4_ = Sound4{};
    std::fill(std::begin(wave_ram_), std::end(wave_ram_), uint8_t{0});
    master_enable_    = r.boolean();
    volume_l_         = r.u8();
    volume_r_         = r.u8();
    ch1_left_enable_  = r.boolean();
    ch1_right_enable_ = r.boolean();
    ch2_left_enable_  = r.boolean();
    ch2_right_enable_ = r.boolean();
    dmg_volume_ratio_ = r.u8();
    r.bytes(&fifo_a_, sizeof(fifo_a_));
    r.bytes(&fifo_b_, sizeof(fifo_b_));
    direct_a_left_        = r.boolean();
    direct_a_right_       = r.boolean();
    direct_a_timer1_      = r.boolean();
    direct_a_full_volume_ = r.boolean();
    direct_b_left_        = r.boolean();
    direct_b_right_       = r.boolean();
    direct_b_timer1_      = r.boolean();
    direct_b_full_volume_ = r.boolean();
    soundbias_         = r.u16();
    cycles_per_sample_ = r.u32();
    cycle_accumulator_ = r.u32();
    uint32_t ring_len = r.u32();
    ring_.assign(ring_len, 0);
    if (ring_len) r.bytes(ring_.data(), ring_len * sizeof(int16_t));
    ring_head_         = static_cast<std::size_t>(r.u64());
    ring_tail_         = static_cast<std::size_t>(r.u64());
    samples_generated_ = r.u64();
    r.bytes(current_samples_, sizeof(current_samples_));
    sample_index_      = r.u32();
}

void GbaAudio::reset() {
    ch1_ = Sound1{};
    ch2_ = Sound2{};
    ch3_ = Sound3{};
    ch4_ = Sound4{};
    std::fill(std::begin(wave_ram_), std::end(wave_ram_), uint8_t{0});
    master_enable_ = false;
    volume_l_ = 7;
    volume_r_ = 7;
    ch1_left_enable_  = false;
    ch1_right_enable_ = false;
    ch2_left_enable_  = false;
    ch2_right_enable_ = false;
    ch3_left_enable_  = false;
    ch3_right_enable_ = false;
    ch4_left_enable_  = false;
    ch4_right_enable_ = false;
    dmg_volume_ratio_ = 1;
    fifo_reset(fifo_a_);
    fifo_reset(fifo_b_);
    direct_a_left_ = false;
    direct_a_right_ = false;
    direct_a_timer1_ = false;
    direct_a_full_volume_ = false;
    direct_b_left_ = false;
    direct_b_right_ = false;
    direct_b_timer1_ = false;
    direct_b_full_volume_ = false;
    soundbias_ = 0x0200;
    cycles_per_sample_ = kDefaultCyclesPerSample;
    // mGBA schedules the GBA audio sample event at reset time and then
    // every 1024 CPU cycles. Prime the accumulator so the first host
    // tick emits that reset-time event, matching the oracle stream.
    cycle_accumulator_ = kSampleEventCycles;
    ring_head_ = ring_tail_ = 0;
    samples_generated_ = 0;
    sample_index_ = 0;
    std::fill(std::begin(current_samples_), std::end(current_samples_),
              int16_t{0});
    trace_write_ = 0;
    trace_count_ = 0;
    std::fill(ring_.begin(), ring_.end(), int16_t{0});
}

void GbaAudio::write_io8(uint32_t off, uint8_t v) {
    // Promote to 16-bit writes by tracking the matching pair byte
    // via an internal cache. For now, treat any 8-bit write to a
    // pair as a partial update — only the byte changed counts.
    // The bus has the canonical store; we just need to react.
    switch (off) {
        // SOUND1CNT_L (0x060..0x061): sweep
        case 0x060: {
            sample_until_current_time();
            ch1_.sweep_shift    = static_cast<uint8_t>(v & 0x7);
            ch1_.sweep_decrease = (v & 0x08) != 0;
            ch1_.sweep_time     = static_cast<uint8_t>((v >> 4) & 0x7);
            break;
        }
        // SOUND1CNT_H (0x062..0x063): length+duty + envelope (same
        // layout as SOUND2CNT_L at 0x068).
        case 0x062: {
            sample_until_current_time();
            ch1_.length = static_cast<uint8_t>(64 - (v & 0x3F));
            ch1_.duty   = static_cast<uint8_t>((v >> 6) & 0x3);
            break;
        }
        case 0x063: {
            sample_until_current_time();
            ch1_.envelope_step     = static_cast<uint8_t>(v & 0x7);
            ch1_.envelope_increase = (v & 0x08) != 0;
            ch1_.envelope_initial  = static_cast<uint8_t>((v >> 4) & 0xF);
            break;
        }
        // SOUND1CNT_X (0x064..0x065): frequency + length-enable + initial
        case 0x064: {
            sample_until_current_time();
            ch1_.frequency = static_cast<uint16_t>(
                (ch1_.frequency & 0x0700) | v);
            break;
        }
        case 0x065: {
            sample_until_current_time();
            ch1_.frequency = static_cast<uint16_t>(
                (ch1_.frequency & 0x00FF) | (static_cast<uint16_t>(v & 0x7) << 8));
            ch1_.length_enabled = (v & 0x40) != 0;
            if (v & 0x80) ch1_trigger();
            break;
        }
        // SOUND2CNT_L (0x068..0x069): length+duty/envelope
        case 0x068: {
            sample_until_current_time();
            ch2_.length = static_cast<uint8_t>(64 - (v & 0x3F));
            ch2_.duty   = static_cast<uint8_t>((v >> 6) & 0x3);
            break;
        }
        case 0x069: {
            sample_until_current_time();
            ch2_.envelope_step    = static_cast<uint8_t>(v & 0x7);
            ch2_.envelope_increase = (v & 0x08) != 0;
            ch2_.envelope_initial = static_cast<uint8_t>((v >> 4) & 0xF);
            // Envelope of 0 with direction "decrease" silences the
            // DAC at next trigger; spec quirk we model on trigger.
            break;
        }
        // SOUND2CNT_H (0x06C..0x06D): frequency + length-enable + initial
        case 0x06C: {
            sample_until_current_time();
            ch2_.frequency = static_cast<uint16_t>(
                (ch2_.frequency & 0x0700) | v);
            break;
        }
        case 0x06D: {
            sample_until_current_time();
            ch2_.frequency = static_cast<uint16_t>(
                (ch2_.frequency & 0x00FF) | (static_cast<uint16_t>(v & 0x7) << 8));
            ch2_.length_enabled = (v & 0x40) != 0;
            if (v & 0x80) ch2_trigger();
            break;
        }
        // ── SOUND3 (wave RAM) ─────────────────────────────────────
        // SOUND3CNT_L (0x070): dimension (bit5), bank (bit6), DAC on (bit7).
        case 0x070: {
            sample_until_current_time();
            ch3_.two_banks = (v & 0x20) != 0;
            ch3_.bank      = static_cast<uint8_t>((v >> 6) & 0x1);
            ch3_.dac_on    = (v & 0x80) != 0;
            if (!ch3_.dac_on) ch3_.active = false;
            break;
        }
        // SOUND3CNT_H (0x072..0x073): length (0x072 bits0-7), volume code
        // (0x073 bits5-6 = SOUND3CNT_H bits13-14), force-75% (0x073 bit7).
        case 0x072: {
            sample_until_current_time();
            ch3_.length = static_cast<uint16_t>(256u - v);
            break;
        }
        case 0x073: {
            sample_until_current_time();
            ch3_.volume_code  = static_cast<uint8_t>((v >> 5) & 0x3);
            ch3_.force_volume = (v & 0x80) != 0;
            break;
        }
        // SOUND3CNT_X (0x074..0x075): frequency + length-enable + trigger.
        case 0x074: {
            sample_until_current_time();
            ch3_.frequency = static_cast<uint16_t>(
                (ch3_.frequency & 0x0700) | v);
            break;
        }
        case 0x075: {
            sample_until_current_time();
            ch3_.frequency = static_cast<uint16_t>(
                (ch3_.frequency & 0x00FF) |
                (static_cast<uint16_t>(v & 0x7) << 8));
            ch3_.length_enabled = (v & 0x40) != 0;
            if (v & 0x80) ch3_trigger();
            break;
        }
        // ── SOUND4 (noise) ────────────────────────────────────────
        // SOUND4CNT_L (0x078): length (bits0-5); 0x079: envelope (same
        // layout as SOUND2CNT_L high byte at 0x069).
        case 0x078: {
            sample_until_current_time();
            ch4_.length = static_cast<uint8_t>(64 - (v & 0x3F));
            break;
        }
        case 0x079: {
            sample_until_current_time();
            ch4_.envelope_step     = static_cast<uint8_t>(v & 0x7);
            ch4_.envelope_increase = (v & 0x08) != 0;
            ch4_.envelope_initial  = static_cast<uint8_t>((v >> 4) & 0xF);
            break;
        }
        // SOUND4CNT_H (0x07C): divisor (bits0-2), width (bit3), shift
        // (bits4-7); 0x07D: length-enable (bit6) + trigger (bit7).
        case 0x07C: {
            sample_until_current_time();
            ch4_.divisor_code = static_cast<uint8_t>(v & 0x7);
            ch4_.width_7bit   = (v & 0x08) != 0;
            ch4_.shift        = static_cast<uint8_t>((v >> 4) & 0xF);
            break;
        }
        case 0x07D: {
            sample_until_current_time();
            ch4_.length_enabled = (v & 0x40) != 0;
            if (v & 0x80) ch4_trigger();
            break;
        }
        // SOUNDCNT_L (0x080..0x081): master L/R volumes + channel routes
        case 0x080: {
            sample_until_current_time();
            volume_r_ = static_cast<uint8_t>(v & 0x7);
            volume_l_ = static_cast<uint8_t>((v >> 4) & 0x7);
            break;
        }
        case 0x081: {
            sample_until_current_time();
            // bits 0..3 = right enables (1=SOUND1, 2=SOUND2, ...)
            // bits 4..7 = left enables
            ch1_right_enable_ = (v & 0x01) != 0;
            ch2_right_enable_ = (v & 0x02) != 0;
            ch3_right_enable_ = (v & 0x04) != 0;
            ch4_right_enable_ = (v & 0x08) != 0;
            ch1_left_enable_  = (v & 0x10) != 0;
            ch2_left_enable_  = (v & 0x20) != 0;
            ch3_left_enable_  = (v & 0x40) != 0;
            ch4_left_enable_  = (v & 0x80) != 0;
            break;
        }
        // SOUNDCNT_H (0x082..0x083): DMG channel volume ratio +
        // SOUND A/B DMA controls. We only model the DMG ratio for
        // now (bits 0..1 of low byte).
        case 0x082: {
            dmg_volume_ratio_ = static_cast<uint8_t>(v & 0x3);
            direct_a_full_volume_ = (v & 0x04) != 0;
            direct_b_full_volume_ = (v & 0x08) != 0;
            break;
        }
        case 0x083: {
            direct_a_right_ = (v & 0x01) != 0;
            direct_a_left_  = (v & 0x02) != 0;
            direct_a_timer1_ = (v & 0x04) != 0;
            if (v & 0x08) fifo_clear_queue(fifo_a_);
            direct_b_right_ = (v & 0x10) != 0;
            direct_b_left_  = (v & 0x20) != 0;
            direct_b_timer1_ = (v & 0x40) != 0;
            if (v & 0x80) fifo_clear_queue(fifo_b_);
            break;
        }
        // SOUNDBIAS (0x088..0x089): bias + output resolution.
        case 0x088: {
            sample_until_current_time();
            update_soundbias(static_cast<uint16_t>(
                (soundbias_ & 0xFF00u) | v));
            break;
        }
        case 0x089: {
            sample_until_current_time();
            update_soundbias(static_cast<uint16_t>(
                (soundbias_ & 0x00FFu) |
                (static_cast<uint16_t>(v) << 8)));
            break;
        }
        // SOUNDCNT_X (0x084): master enable + per-channel ON flags
        case 0x084: {
            sample_until_current_time();
            master_enable_ = (v & 0x80) != 0;
            if (!master_enable_) {
                // Disabling kills all channel state per hardware.
                ch1_.active = false;
                ch2_.active = false;
                ch3_.active = false;
                ch4_.active = false;
            }
            break;
        }
        default:
            // Wave RAM (0x090..0x09F): the CPU window maps to the bank NOT
            // selected for playback (SOUND3CNT_L bit6), so a game can fill the
            // next bank while the current one plays (mGBA model). wave_ram_ is
            // 32 bytes = two 16-byte banks (bank0 = bytes 0-15, bank1 = 16-31).
            if (off >= 0x090 && off <= 0x09F) {
                sample_until_current_time();
                uint32_t bank_base = (ch3_.bank ^ 1u) ? 16u : 0u;
                wave_ram_[bank_base + (off - 0x090u)] = v;
            }
            break;
    }
}

void GbaAudio::write_io16(uint32_t off, uint16_t v) {
    write_io8(off,     static_cast<uint8_t>(v & 0xFF));
    write_io8(off + 1, static_cast<uint8_t>((v >> 8) & 0xFF));
}

void GbaAudio::write_io32(uint32_t off, uint32_t v) {
    if (off == 0x0A0) {
        fifo_push_word(fifo_a_, v);
        return;
    }
    if (off == 0x0A4) {
        fifo_push_word(fifo_b_, v);
        return;
    }
    write_io16(off,     static_cast<uint16_t>(v & 0xFFFF));
    write_io16(off + 2, static_cast<uint16_t>((v >> 16) & 0xFFFF));
}

void GbaAudio::ch1_trigger() {
    ch1_.active = true;
    if (ch1_.length == 0) ch1_.length = 64;
    ch1_.volume          = ch1_.envelope_initial;
    ch1_.waveform_cycles = 0;
    ch1_.waveform_phase  = 0;
    ch1_.envelope_cycles = 0;
    ch1_.length_cycles   = 0;
    ch1_.sweep_cycles    = 0;
    if (ch1_.envelope_initial == 0 && !ch1_.envelope_increase) {
        ch1_.active = false;
    }
}

void GbaAudio::ch2_trigger() {
    ch2_.active = true;
    if (ch2_.length == 0) ch2_.length = 64;
    ch2_.volume          = ch2_.envelope_initial;
    ch2_.waveform_cycles = 0;
    ch2_.waveform_phase  = 0;
    ch2_.envelope_cycles = 0;
    ch2_.length_cycles   = 0;
    // A DAC-disabled trigger (init volume 0, decrease envelope) is
    // still considered "started" by the channel but emits silence.
    if (ch2_.envelope_initial == 0 && !ch2_.envelope_increase) {
        ch2_.active = false;
    }
}

void GbaAudio::ch3_trigger() {
    // SOUND3CNT_X bit15. With the DAC off (bit7 of SOUND3CNT_L) the channel
    // produces no output regardless of the trigger.
    if (!ch3_.dac_on) { ch3_.active = false; return; }
    ch3_.active = true;
    if (ch3_.length == 0) ch3_.length = 256;
    ch3_.wave_pos    = 0;   // restart at the first digit of the selected bank
    ch3_.wave_cycles = 0;
    ch3_.length_cycles = 0;
}

void GbaAudio::ch4_trigger() {
    ch4_.active = true;
    if (ch4_.length == 0) ch4_.length = 64;
    ch4_.volume          = ch4_.envelope_initial;
    ch4_.lfsr            = 0x7FFF;   // all-ones reload per hardware
    ch4_.noise_cycles    = 0;
    ch4_.envelope_cycles = 0;
    ch4_.length_cycles   = 0;
    // DAC-disabled trigger (init vol 0, decreasing envelope) → silent.
    if (ch4_.envelope_initial == 0 && !ch4_.envelope_increase) {
        ch4_.active = false;
    }
}

void GbaAudio::ring_push(int16_t s) {
    ring_[ring_head_] = s;
    ring_head_ = (ring_head_ + 1) % kRingSize;
    if (ring_head_ == ring_tail_) {
        // Overrun — drop oldest by advancing tail.
        ring_tail_ = (ring_tail_ + 1) % kRingSize;
    }
    ++samples_generated_;
}

std::size_t GbaAudio::drain_samples(int16_t* out, std::size_t max) {
    std::size_t n = 0;
    while (n < max && ring_tail_ != ring_head_) {
        out[n++] = ring_[ring_tail_];
        ring_tail_ = (ring_tail_ + 1) % kRingSize;
    }
    return n;
}

void GbaAudio::fifo_reset(DirectFifo& fifo) {
    fifo = DirectFifo{};
}

void GbaAudio::fifo_clear_queue(DirectFifo& fifo) {
    fifo.write = 0;
    fifo.read = 0;
    fifo.count = 0;
}

void GbaAudio::fifo_push_word(DirectFifo& fifo, uint32_t word) {
    fifo.words[fifo.write] = word;
    fifo.write = static_cast<uint8_t>((fifo.write + 1) & 7u);
    if (fifo.count < 8) {
        ++fifo.count;
    } else {
        fifo.read = static_cast<uint8_t>((fifo.read + 1) & 7u);
    }
}

void GbaAudio::fifo_timer_step(DirectFifo& fifo, int fifo_id) {
    if (fifo.bytes_remaining == 0 && fifo.count != 0) {
        fifo.shift_word = fifo.words[fifo.read];
        fifo.read = static_cast<uint8_t>((fifo.read + 1) & 7u);
        --fifo.count;
        fifo.bytes_remaining = 4;
    }
    int8_t sample = static_cast<int8_t>(fifo.shift_word & 0xFFu);
    uint32_t slots = samples_per_event();
    uint32_t until_cycles = cycles_until_next_sample_event();
    uint32_t until = ((until_cycles - 1u) + cycles_per_sample_) /
        cycles_per_sample_;
    if (until > slots) until = slots;
    uint32_t start = slots - until;
    for (uint32_t i = start; i < slots; ++i) {
        fifo.samples[i] = sample;
    }
    FifoTrace& trace = trace_[trace_write_];
    trace.sample_base = samples_generated_;
    trace.fifo_id = static_cast<uint8_t>(fifo_id);
    trace.until_cycles = until_cycles;
    trace.start_slot = start;
    trace.slots = slots;
    trace.count = fifo.count;
    trace.bytes_remaining = fifo.bytes_remaining;
    trace.sample = sample;
    trace_write_ = (trace_write_ + 1u) % kFifoTraceSize;
    if (trace_count_ < kFifoTraceSize) ++trace_count_;
    if (fifo.bytes_remaining != 0) {
        fifo.shift_word >>= 8;
        --fifo.bytes_remaining;
    }
}

void GbaAudio::update_soundbias(uint16_t value) {
    soundbias_ = value;
    uint32_t resolution = (value >> 14) & 0x3u;
    cycles_per_sample_ = 0x200u >> resolution;
    if (cycles_per_sample_ == 0) cycles_per_sample_ = 1;
}

void GbaAudio::timer_overflow(int timer_id) {
    if (timer_id == (direct_a_timer1_ ? 1 : 0)) {
        fifo_timer_step(fifo_a_, 0);
    }
    if (timer_id == (direct_b_timer1_ ? 1 : 0)) {
        fifo_timer_step(fifo_b_, 1);
    }
}

bool GbaAudio::fifo_needs_dma(int fifo_id) const {
    const DirectFifo& fifo = (fifo_id == 0) ? fifo_a_ : fifo_b_;
    return fifo.count < 4;
}

GbaAudio::FifoDebugState GbaAudio::debug_fifo_state(int fifo_id) const {
    const DirectFifo& fifo = (fifo_id == 0) ? fifo_a_ : fifo_b_;
    FifoDebugState out{};
    out.write = fifo.write;
    out.read = fifo.read;
    out.count = fifo.count;
    out.shift_word = fifo.shift_word;
    out.bytes_remaining = fifo.bytes_remaining;
    for (uint32_t i = 0; i < kMaxSamplesPerEvent; ++i) {
        out.samples[i] = fifo.samples[i];
    }
    return out;
}

GbaAudio::FifoTrace GbaAudio::debug_trace_entry(uint32_t index) const {
    if (index >= trace_count_) return FifoTrace{};
    uint32_t start = (trace_write_ + kFifoTraceSize - trace_count_) %
        kFifoTraceSize;
    return trace_[(start + index) % kFifoTraceSize];
}

// ─────────────────────────────────────────────────────────────────────
// Sample generation
// ─────────────────────────────────────────────────────────────────────

uint32_t GbaAudio::samples_per_event() const {
    uint32_t samples = kSampleEventCycles / cycles_per_sample_;
    if (samples == 0) samples = 1;
    if (samples > kMaxSamplesPerEvent) samples = kMaxSamplesPerEvent;
    return samples;
}

uint32_t GbaAudio::cycles_until_next_sample_event() const {
    if (cycle_accumulator_ >= kSampleEventCycles) return 1;
    uint32_t remaining = kSampleEventCycles - cycle_accumulator_;
    return remaining ? remaining : 1;
}

void GbaAudio::sample_until_current_time() {
    uint32_t slots = samples_per_event();
    uint32_t elapsed = cycle_accumulator_ / cycles_per_sample_;
    if (elapsed > slots) elapsed = slots;
    while (sample_index_ < elapsed) {
        current_samples_[sample_index_] = mix_one_sample(sample_index_);
        ++sample_index_;
    }
}

int16_t GbaAudio::mix_one_sample(uint32_t direct_slot, bool include_direct) {
    if (!master_enable_) return 0;

    // Per-channel contributions in the range [-15, +15]. Routing
    // bits gate whether the channel contributes to either side; we
    // mix mono for now and treat "either side enabled" as "audible".
    int32_t ch1_sample = 0;
    int32_t ch2_sample = 0;

    // ── Channel 1 (square + sweep) ───────────────────────────────
    if (ch1_.active) {
        uint32_t freq_period = (2048u - ch1_.frequency) * 16u;
        if (freq_period == 0) freq_period = 16u;
        ch1_.waveform_cycles += cycles_per_sample_;
        while (ch1_.waveform_cycles >= freq_period) {
            ch1_.waveform_cycles -= freq_period;
            ch1_.waveform_phase = (ch1_.waveform_phase + 1) & 7;
        }
        if (ch1_.envelope_step != 0) {
            ++ch1_.envelope_cycles;
            uint32_t env_period = ch1_.envelope_step * (sample_rate() / 64u);
            if (ch1_.envelope_cycles >= env_period) {
                ch1_.envelope_cycles = 0;
                if (ch1_.envelope_increase) {
                    if (ch1_.volume < 15) ++ch1_.volume;
                } else {
                    if (ch1_.volume > 0) --ch1_.volume;
                }
            }
        }
        if (ch1_.length_enabled) {
            ++ch1_.length_cycles;
            if (ch1_.length_cycles >= (sample_rate() / 256u)) {
                ch1_.length_cycles = 0;
                if (ch1_.length > 0) {
                    --ch1_.length;
                    if (ch1_.length == 0) ch1_.active = false;
                }
            }
        }
        // Sweep clock: 128 Hz (every 1/128 s adjust frequency by
        // freq +/- (freq >> shift)). sweep_time=0 disables.
        if (ch1_.sweep_time != 0) {
            ++ch1_.sweep_cycles;
            uint32_t sweep_period = ch1_.sweep_time * (sample_rate() / 128u);
            if (ch1_.sweep_cycles >= sweep_period) {
                ch1_.sweep_cycles = 0;
                uint16_t delta = static_cast<uint16_t>(
                    ch1_.frequency >> ch1_.sweep_shift);
                if (ch1_.sweep_decrease) {
                    if (delta <= ch1_.frequency) ch1_.frequency -= delta;
                } else {
                    if (ch1_.frequency + delta > 2047) {
                        // Sweep overflow disables the channel per spec.
                        ch1_.active = false;
                    } else {
                        ch1_.frequency = static_cast<uint16_t>(
                            ch1_.frequency + delta);
                    }
                }
            }
        }
        if (ch1_.active) {
            uint8_t high = kDutyPatterns[ch1_.duty][ch1_.waveform_phase];
            ch1_sample = (high ? 1 : -1) * static_cast<int32_t>(ch1_.volume);
        }
    }

    // ── Channel 2 ────────────────────────────────────────────────
    if (ch2_.active) {
        // Advance the waveform timer. The square-wave clock is
        // 131072 Hz scaled by (2048 - frequency). One waveform
        // period is 8 phases, so the per-phase increment in cycles
        // is (2048 - freq) * 16. (Per GBATEK § "GBA Sound Channel 2".)
        uint32_t freq_period = (2048u - ch2_.frequency) * 16u;
        ch2_.waveform_cycles += cycles_per_sample_;
        while (ch2_.waveform_cycles >= freq_period) {
            ch2_.waveform_cycles -= freq_period;
            ch2_.waveform_phase = (ch2_.waveform_phase + 1) & 7;
        }
        // Envelope clock: 1/64 second per step = kSampleRate/64 = 512
        // sample-ticks per step.
        if (ch2_.envelope_step != 0) {
            ++ch2_.envelope_cycles;
            uint32_t env_period = ch2_.envelope_step * (sample_rate() / 64u);
            if (ch2_.envelope_cycles >= env_period) {
                ch2_.envelope_cycles = 0;
                if (ch2_.envelope_increase) {
                    if (ch2_.volume < 15) ++ch2_.volume;
                } else {
                    if (ch2_.volume > 0) --ch2_.volume;
                }
            }
        }
        // Length clock: 256 Hz = kSampleRate/256 = 128 sample-ticks.
        if (ch2_.length_enabled) {
            ++ch2_.length_cycles;
            if (ch2_.length_cycles >= (sample_rate() / 256u)) {
                ch2_.length_cycles = 0;
                if (ch2_.length > 0) {
                    --ch2_.length;
                    if (ch2_.length == 0) ch2_.active = false;
                }
            }
        }
        if (ch2_.active) {
            uint8_t high = kDutyPatterns[ch2_.duty][ch2_.waveform_phase];
            ch2_sample = (high ? 1 : -1) * static_cast<int32_t>(ch2_.volume);
        }
    }

    // ── Channel 3 (wave RAM, 4-bit PCM) ──────────────────────────
    int32_t ch3_sample = 0;
    if (ch3_.active && ch3_.dac_on) {
        // One 4-bit digit advances every (2048-freq)*8 cycles (32 digits per
        // waveform; half ch1/ch2's per-phase period since the wave has 4× the
        // steps). Per GBATEK § "GBA Sound Channel 3".
        uint32_t digit_period = (2048u - ch3_.frequency) * 8u;
        if (digit_period == 0) digit_period = 8u;
        uint32_t digits = ch3_.two_banks ? 64u : 32u;
        ch3_.wave_cycles += cycles_per_sample_;
        while (ch3_.wave_cycles >= digit_period) {
            ch3_.wave_cycles -= digit_period;
            ch3_.wave_pos = static_cast<uint8_t>((ch3_.wave_pos + 1u) % digits);
        }
        if (ch3_.length_enabled) {
            ++ch3_.length_cycles;
            if (ch3_.length_cycles >= (sample_rate() / 256u)) {
                ch3_.length_cycles = 0;
                if (ch3_.length > 0) {
                    --ch3_.length;
                    if (ch3_.length == 0) ch3_.active = false;
                }
            }
        }
        if (ch3_.active) {
            uint32_t abs_digit =
                (static_cast<uint32_t>(ch3_.bank) * 32u + ch3_.wave_pos) & 63u;
            uint8_t byte = wave_ram_[abs_digit >> 1];
            uint8_t digit = (abs_digit & 1u) ? (byte & 0x0Fu)
                                             : static_cast<uint8_t>(byte >> 4);
            // Volume: force-75% overrides the 2-bit code
            // (0=mute,1=100%,2=50%,3=25%).
            int32_t num, den;
            if (ch3_.force_volume) { num = 3; den = 4; }
            else switch (ch3_.volume_code) {
                case 0:  num = 0; den = 1; break;
                case 1:  num = 1; den = 1; break;
                case 2:  num = 1; den = 2; break;
                default: num = 1; den = 4; break;
            }
            // 4-bit unsigned digit → signed swing ±15 (matching ch1/ch2 max),
            // scaled by the volume fraction.
            ch3_sample = ((2 * static_cast<int32_t>(digit) - 15) * num) / den;
        }
    }

    // ── Channel 4 (noise, LFSR) ──────────────────────────────────
    int32_t ch4_sample = 0;
    if (ch4_.active) {
        // LFSR steps at 524288 / r / 2^(s+1) Hz: GB divisor table {8,16,…,112}
        // (r=0 → 8) shifted left by s, ×4 for the GBA's 4× system clock.
        static constexpr uint32_t kNoiseDiv[8] =
            {8u, 16u, 32u, 48u, 64u, 80u, 96u, 112u};
        uint32_t step_period =
            (kNoiseDiv[ch4_.divisor_code] << ch4_.shift) * 4u;
        if (step_period == 0) step_period = 4u;
        ch4_.noise_cycles += cycles_per_sample_;
        while (ch4_.noise_cycles >= step_period) {
            ch4_.noise_cycles -= step_period;
            uint32_t fb = (ch4_.lfsr ^ (ch4_.lfsr >> 1)) & 1u;
            ch4_.lfsr = static_cast<uint16_t>((ch4_.lfsr >> 1) | (fb << 14));
            if (ch4_.width_7bit) {
                ch4_.lfsr = static_cast<uint16_t>(
                    (ch4_.lfsr & ~(1u << 6)) | (fb << 6));
            }
        }
        if (ch4_.envelope_step != 0) {
            ++ch4_.envelope_cycles;
            uint32_t env_period = ch4_.envelope_step * (sample_rate() / 64u);
            if (ch4_.envelope_cycles >= env_period) {
                ch4_.envelope_cycles = 0;
                if (ch4_.envelope_increase) {
                    if (ch4_.volume < 15) ++ch4_.volume;
                } else {
                    if (ch4_.volume > 0) --ch4_.volume;
                }
            }
        }
        if (ch4_.length_enabled) {
            ++ch4_.length_cycles;
            if (ch4_.length_cycles >= (sample_rate() / 256u)) {
                ch4_.length_cycles = 0;
                if (ch4_.length > 0) {
                    --ch4_.length;
                    if (ch4_.length == 0) ch4_.active = false;
                }
            }
        }
        if (ch4_.active) {
            // Output is high when LFSR bit 0 is 0.
            int32_t high = ((ch4_.lfsr & 1u) == 0) ? 1 : -1;
            ch4_sample = high * static_cast<int32_t>(ch4_.volume);
        }
    }

    // Apply routing — channel must be enabled on at least one side
    // to be audible in our mono mix.
    int32_t mix = 0;
    if (ch1_left_enable_ || ch1_right_enable_) mix += ch1_sample;
    if (ch2_left_enable_ || ch2_right_enable_) mix += ch2_sample;
    if (ch3_left_enable_ || ch3_right_enable_) mix += ch3_sample;
    if (ch4_left_enable_ || ch4_right_enable_) mix += ch4_sample;

    // DMG volume ratio (SOUNDCNT_H bits 0..1): 0=25% 1=50% 2=100%.
    // We bake this into the output scale.
    int32_t dmg_scale_num = 1;
    int32_t dmg_scale_den = 4;
    switch (dmg_volume_ratio_) {
        case 0: dmg_scale_num = 1; dmg_scale_den = 4; break;  // 25%
        case 1: dmg_scale_num = 1; dmg_scale_den = 2; break;  // 50%
        case 2: dmg_scale_num = 1; dmg_scale_den = 1; break;  // 100%
        default: dmg_scale_num = 0; dmg_scale_den = 1; break; // prohibited
    }

    // Apply master left/right volume; we average L and R for mono.
    // SOUNDCNT_L master volume is 0..7 (1/8 per step, with 7 being
    // full).
    int32_t master_avg = (volume_l_ + volume_r_);  // 0..14
    // Combine: mix * (master_avg / 14) * (dmg / 4) * gain.
    // Mix range: ±30 (both ch active at vol 15). Master 14 (max).
    // dmg 1/1. Pre-gain magnitude: 30 * 14 / 14 * 1 = 30. We need
    // headroom for int16; scale by ~1000 leaves margin.
    int32_t out = (mix * master_avg) / 14;
    out = (out * dmg_scale_num) / dmg_scale_den;
    constexpr int32_t kGain = 600;
    out *= kGain;

    // Direct Sound FIFO contribution. Omitted when the MP2K shadow is
    // substituting (the caller adds the shadow's direct mix instead).
    if (include_direct) {
        int32_t direct_left = 0;
        int32_t direct_right = 0;
        auto add_direct = [&](const DirectFifo& fifo, bool left, bool right,
                              bool full_volume) {
            int32_t scale = full_volume ? 4 : 2;
            int32_t sample =
                static_cast<int32_t>(fifo.samples[direct_slot]) * scale;
            if (left) direct_left += sample;
            if (right) direct_right += sample;
        };
        add_direct(fifo_a_, direct_a_left_, direct_a_right_,
                   direct_a_full_volume_);
        add_direct(fifo_b_, direct_b_left_, direct_b_right_,
                   direct_b_full_volume_);
        int32_t direct_out = ((direct_left + direct_right) / 2) * 48;
        out += direct_out;
    }

    if (out > 32767)  out = 32767;
    if (out < -32767) out = -32767;
    return static_cast<int16_t>(out);
}

void GbaAudio::configure_shadow(const std::vector<Mp2kSig>& sigs,
                                const uint8_t* rom, std::size_t rom_len,
                                const uint8_t* ewram, std::size_t ewram_len,
                                const uint8_t* iwram, std::size_t iwram_len,
                                bool enable_request) {
    shadow_mem_.rom = rom;       shadow_mem_.rom_len = rom_len;
    shadow_mem_.ewram = ewram;   shadow_mem_.ewram_len = ewram_len;
    shadow_mem_.iwram = iwram;   shadow_mem_.iwram_len = iwram_len;
    shadow_.init(sigs);
    shadow_cursor_ = 0;
    shadow_samples_since_hook_ = 0;
    shadow_hook_period_ = sample_rate() / 60u;
    if (shadow_hook_period_ == 0) shadow_hook_period_ = 1097u;
    // Opt-in. `enable_request` is the per-game default ([audio].shadow in
    // game.toml); GBARECOMP_AUDIO_SHADOW overrides it ("0" forces off, any
    // other value forces on). Only takes effect if the ROM links MP2K.
    bool want = enable_request;
    if (const char* e = std::getenv("GBARECOMP_AUDIO_SHADOW")) {
        want = !(e[0] == '0' && e[1] == '\0');
    }
    shadow_enabled_ = shadow_.armed() && want;
    if (shadow_enabled_) {
        std::fprintf(stderr, "[audio] MP2K shadow mixer ARMED (verified-"
                             "enhancement; reverts to hardware mix on divergence)\n");
    }
}

void GbaAudio::run_sample_event() {
    uint32_t samples = samples_per_event();
    while (sample_index_ < samples) {
        uint32_t slot = sample_index_;
        bool substitute = false;
        float sl = 0.0f, sr = 0.0f;
        if (shadow_enabled_) {
            std::string degraded;
            shadow_.render(shadow_mem_, fifo_a_.samples[slot],
                           fifo_b_.samples[slot], sl, sr, degraded);
            if (!degraded.empty()) {
                std::fprintf(stderr, "[audio] MP2K shadow DEGRADED: %s\n",
                             degraded.c_str());
            }
            ++shadow_cursor_;
            if (++shadow_samples_since_hook_ >= shadow_hook_period_) {
                shadow_.frame_hook(shadow_mem_, shadow_cursor_,
                                   shadow_.hook_key0());
                shadow_samples_since_hook_ = 0;
            }
            substitute = shadow_.live();
        }
        // When the shadow is proven-live, mix PSG-only canon + the shadow's
        // direct-sound render; otherwise the full canon mix (incl. FIFO).
        int16_t s = mix_one_sample(slot, /*include_direct=*/!substitute);
        if (substitute) {
            // Shadow output is the bus float domain (full-scale FIFO DAC =
            // 0.25). kShadowOutputScale maps it to our int16 direct-sound
            // magnitude — the loudness-match knob to tune in the audio
            // oracle validation pass.
            constexpr float kShadowOutputScale = 24000.0f;
            int32_t shadow_mix =
                static_cast<int32_t>(((sl + sr) * 0.5f) * kShadowOutputScale);
            int32_t o = static_cast<int32_t>(s) + shadow_mix;
            if (o > 32767) o = 32767;
            if (o < -32767) o = -32767;
            s = static_cast<int16_t>(o);
        }
        current_samples_[slot] = s;
        ++sample_index_;
    }
    for (uint32_t i = 0; i < samples; ++i) {
        ring_push(current_samples_[i]);
    }
    sample_index_ = 0;

    int8_t last_a = fifo_a_.samples[samples - 1u];
    int8_t last_b = fifo_b_.samples[samples - 1u];
    std::fill(std::begin(fifo_a_.samples), std::end(fifo_a_.samples), last_a);
    std::fill(std::begin(fifo_b_.samples), std::end(fifo_b_.samples), last_b);
}

void GbaAudio::tick(uint32_t cycles) {
    cycle_accumulator_ += cycles;
    while (cycle_accumulator_ >= kSampleEventCycles) {
        cycle_accumulator_ -= kSampleEventCycles;
        run_sample_event();
    }
}

uint32_t GbaAudio::cycles_until_next_sample() const {
    return cycles_until_next_sample_event();
}

}  // namespace gba
