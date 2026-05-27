// gba_audio.cpp — see gba_audio.h. Phase 2.7.C: minimal SOUND2 + mixer.

#include "gba_audio.h"

#include <algorithm>

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
    master_enable_ = false;
    volume_l_ = 7;
    volume_r_ = 7;
    ch1_left_enable_  = false;
    ch1_right_enable_ = false;
    ch2_left_enable_  = false;
    ch2_right_enable_ = false;
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
            ch1_left_enable_  = (v & 0x10) != 0;
            ch2_left_enable_  = (v & 0x20) != 0;
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
            }
            break;
        }
        default:
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

int16_t GbaAudio::mix_one_sample(uint32_t direct_slot) {
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

    // Apply routing — channel must be enabled on at least one side
    // to be audible in our mono mix.
    int32_t mix = 0;
    if (ch1_left_enable_ || ch1_right_enable_) mix += ch1_sample;
    if (ch2_left_enable_ || ch2_right_enable_) mix += ch2_sample;

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

    if (out > 32767)  out = 32767;
    if (out < -32767) out = -32767;
    return static_cast<int16_t>(out);
}

void GbaAudio::run_sample_event() {
    uint32_t samples = samples_per_event();
    while (sample_index_ < samples) {
        current_samples_[sample_index_] = mix_one_sample(sample_index_);
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
