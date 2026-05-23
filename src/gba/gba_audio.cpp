// gba_audio.cpp — see gba_audio.h. Phase 2.7.C: minimal SOUND2 + mixer.

#include "gba_audio.h"

#include <algorithm>

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
}
GbaAudio::~GbaAudio() = default;

void GbaAudio::reset() {
    ch2_ = Sound2{};
    master_enable_ = false;
    volume_l_ = 7;
    volume_r_ = 7;
    ch2_left_enable_  = false;
    ch2_right_enable_ = false;
    cycle_accumulator_ = 0;
    ring_head_ = ring_tail_ = 0;
    samples_generated_ = 0;
    std::fill(ring_.begin(), ring_.end(), int16_t{0});
}

void GbaAudio::write_io8(uint32_t off, uint8_t v) {
    // Promote to 16-bit writes by tracking the matching pair byte
    // via an internal cache. For now, treat any 8-bit write to a
    // pair as a partial update — only the byte changed counts.
    // The bus has the canonical store; we just need to react.
    switch (off) {
        // SOUND2CNT_L (0x068..0x069): length+duty/envelope
        case 0x068: {
            ch2_.length = static_cast<uint8_t>(64 - (v & 0x3F));
            ch2_.duty   = static_cast<uint8_t>((v >> 6) & 0x3);
            break;
        }
        case 0x069: {
            ch2_.envelope_step    = static_cast<uint8_t>(v & 0x7);
            ch2_.envelope_increase = (v & 0x08) != 0;
            ch2_.envelope_initial = static_cast<uint8_t>((v >> 4) & 0xF);
            // Envelope of 0 with direction "decrease" silences the
            // DAC at next trigger; spec quirk we model on trigger.
            break;
        }
        // SOUND2CNT_H (0x06C..0x06D): frequency + length-enable + initial
        case 0x06C: {
            ch2_.frequency = static_cast<uint16_t>(
                (ch2_.frequency & 0x0700) | v);
            break;
        }
        case 0x06D: {
            ch2_.frequency = static_cast<uint16_t>(
                (ch2_.frequency & 0x00FF) | (static_cast<uint16_t>(v & 0x7) << 8));
            ch2_.length_enabled = (v & 0x40) != 0;
            if (v & 0x80) ch2_trigger();
            break;
        }
        // SOUNDCNT_L (0x080..0x081): master L/R volumes + channel routes
        case 0x080: {
            volume_r_ = static_cast<uint8_t>(v & 0x7);
            volume_l_ = static_cast<uint8_t>((v >> 4) & 0x7);
            break;
        }
        case 0x081: {
            // bits 0..3 = right enables (1=SOUND1, 2=SOUND2, ...)
            // bits 4..7 = left enables
            ch2_right_enable_ = (v & 0x02) != 0;
            ch2_left_enable_  = (v & 0x20) != 0;
            break;
        }
        // SOUNDCNT_X (0x084): master enable + per-channel ON flags
        case 0x084: {
            master_enable_ = (v & 0x80) != 0;
            if (!master_enable_) {
                // Disabling kills all channel state per hardware.
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
    write_io16(off,     static_cast<uint16_t>(v & 0xFFFF));
    write_io16(off + 2, static_cast<uint16_t>((v >> 16) & 0xFFFF));
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

// ─────────────────────────────────────────────────────────────────────
// Sample generation
// ─────────────────────────────────────────────────────────────────────

int16_t GbaAudio::mix_one_sample() {
    if (!master_enable_) return 0;

    int32_t mix = 0;

    // ── Channel 2 ────────────────────────────────────────────────
    if (ch2_.active) {
        // Advance the waveform timer. The square-wave clock is
        // 131072 Hz scaled by (2048 - frequency). One waveform
        // period is 8 phases, so the per-phase increment in cycles
        // is (2048 - freq) * 16. (Per GBATEK § "GBA Sound Channel 2".)
        uint32_t freq_period = (2048u - ch2_.frequency) * 16u;
        ch2_.waveform_cycles += kCyclesPerSample;
        while (ch2_.waveform_cycles >= freq_period) {
            ch2_.waveform_cycles -= freq_period;
            ch2_.waveform_phase = (ch2_.waveform_phase + 1) & 7;
        }
        // Envelope clock: 1/64 second per step = kSampleRate/64 = 512
        // sample-ticks per step.
        if (ch2_.envelope_step != 0) {
            ++ch2_.envelope_cycles;
            uint32_t env_period = ch2_.envelope_step * (kSampleRate / 64u);
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
            if (ch2_.length_cycles >= (kSampleRate / 256u)) {
                ch2_.length_cycles = 0;
                if (ch2_.length > 0) {
                    --ch2_.length;
                    if (ch2_.length == 0) ch2_.active = false;
                }
            }
        }
        if (ch2_.active) {
            uint8_t high = kDutyPatterns[ch2_.duty][ch2_.waveform_phase];
            int32_t s = (high ? 1 : -1) * static_cast<int32_t>(ch2_.volume);
            mix += s;
        }
    }

    // 16-bit scale. ch2 alone produces ±15. Scale by ~1000 for room
    // among the other channels we'll mix in later.
    return static_cast<int16_t>(std::max(-32767, std::min(32767, mix * 800)));
}

void GbaAudio::tick(uint32_t cycles) {
    cycle_accumulator_ += cycles;
    while (cycle_accumulator_ >= kCyclesPerSample) {
        cycle_accumulator_ -= kCyclesPerSample;
        ring_push(mix_one_sample());
    }
}

}  // namespace gba
