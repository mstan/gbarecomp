// gba_audio.h — GBA sound channels + sample mixer.
//
// Phase 2.7.C scope: SOUND2 (square wave, no sweep), enough to play
// the BIOS startup chime. SOUND1 / SOUND3 / SOUND4 and the DMA-driven
// SOUND A / SOUND B FIFOs are stubbed in — they accept IO writes but
// produce no output yet. The mixer generates 16-bit signed samples at
// ~32.768 kHz (one sample per 512 system cycles).
//
// References:
//   GBATEK § "GBA Sound Channel 2 - Tone"
//   GBATEK § "GBA Sound Channels 1-4 — Tone Generation"
//   GBATEK § "GBA Sound Controller" (SOUNDCNT_L/H/X master)

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gba {

class GbaAudio {
public:
    GbaAudio();
    ~GbaAudio();

    // GBA runs at 16.777216 MHz; we sample at exactly 16777216 / 512 =
    // 32768 Hz so timing math is integer-clean.
    static constexpr uint32_t kSystemHz       = 16777216u;
    static constexpr uint32_t kDefaultSampleRate = 32768u;
    static constexpr uint32_t kDefaultCyclesPerSample =
        kSystemHz / kDefaultSampleRate;  // 512
    static constexpr uint32_t kSampleEventCycles = 1024u;
    static constexpr uint32_t kMaxSamplesPerEvent = 16u;

    // Reset all channel state. Called by the bus on power-on.
    void reset();

    // Write to an audio IO register. `off` is relative to 0x04000000
    // (so SOUND1CNT_L is 0x060, SOUND2CNT_L is 0x068, etc.). The
    // backing IO array still stores the byte; this method updates
    // the channel state machines for side effects (envelope reload,
    // length reset, frequency re-trigger).
    void write_io8 (uint32_t off, uint8_t  v);
    void write_io16(uint32_t off, uint16_t v);
    void write_io32(uint32_t off, uint32_t v);

    // Advance the audio subsystem by `cycles` system cycles.
    // Generates samples into the output ring whenever the cycle
    // accumulator crosses the active SOUNDBIAS sample interval.
    void tick(uint32_t cycles);
    uint32_t cycles_until_next_sample() const;

    // Drain up to `max` samples from the ring into `out`. Returns
    // the number actually written. Used by the host audio backend
    // and by the TCP audio_samples command.
    std::size_t drain_samples(int16_t* out, std::size_t max);

    uint32_t sample_rate() const { return kSystemHz / cycles_per_sample_; }

    // Total samples generated since reset — useful for sync diff.
    uint64_t samples_generated() const { return samples_generated_; }

    struct FifoDebugState {
        uint8_t write = 0;
        uint8_t read = 0;
        uint8_t count = 0;
        uint32_t shift_word = 0;
        uint8_t bytes_remaining = 0;
        int8_t samples[kMaxSamplesPerEvent] = {};
    };
    FifoDebugState debug_fifo_state(int fifo_id) const;
    uint32_t debug_cycle_accumulator() const { return cycle_accumulator_; }
    uint32_t debug_samples_per_event() const { return samples_per_event(); }
    uint32_t debug_cycles_until_event() const {
        return cycles_until_next_sample_event();
    }
    uint16_t debug_soundbias() const { return soundbias_; }

    struct FifoTrace {
        uint64_t sample_base = 0;
        uint8_t fifo_id = 0;
        uint32_t until_cycles = 0;
        uint32_t start_slot = 0;
        uint32_t slots = 0;
        uint8_t count = 0;
        uint8_t bytes_remaining = 0;
        int8_t sample = 0;
    };
    static constexpr uint32_t kFifoTraceSize = 1024u;
    uint32_t debug_trace_count() const { return trace_count_; }
    FifoTrace debug_trace_entry(uint32_t index) const;

    // Timer overflow hook for direct sound FIFO A/B. `timer_id` is
    // 0 or 1; SOUNDCNT_H selects which timer clocks each FIFO.
    void timer_overflow(int timer_id);
    bool fifo_needs_dma(int fifo_id) const;

private:
    // ── Channel 1: square wave with frequency sweep ──────────────
    struct Sound1 {
        bool     active = false;
        uint8_t  duty = 2;
        uint8_t  length = 64;
        bool     length_enabled = false;
        uint8_t  envelope_initial = 0;
        bool     envelope_increase = false;
        uint8_t  envelope_step  = 0;
        uint8_t  volume = 0;
        uint16_t frequency = 0;
        uint32_t waveform_cycles = 0;
        uint8_t  waveform_phase  = 0;
        uint32_t envelope_cycles = 0;
        uint32_t length_cycles   = 0;
        // Sweep state (SOUND1 only — the BIOS chime uses sweep).
        uint8_t  sweep_shift     = 0;   // 0..7
        bool     sweep_decrease  = false;
        uint8_t  sweep_time      = 0;   // 0..7 (0 = disabled)
        uint32_t sweep_cycles    = 0;
    };
    Sound1 ch1_{};

    // ── Channel 2: square wave ────────────────────────────────────
    struct Sound2 {
        // Live envelope + length state.
        bool     active = false;            // playing
        uint8_t  duty = 2;                  // 0..3 (12.5/25/50/75 % high)
        uint8_t  length = 64;               // remaining length (0..64); 0 = stop if length_enabled
        bool     length_enabled = false;    // bit 14 of SOUND2CNT_H
        uint8_t  envelope_initial = 0;      // 0..15 starting volume
        bool     envelope_increase = false; // direction
        uint8_t  envelope_step  = 0;        // 0..7 (period in 1/64 s steps)
        uint8_t  volume = 0;                // current envelope volume 0..15
        uint16_t frequency = 0;             // 11-bit "frequency" field
        // Internal cycle accumulators (in 1/kSampleRate units for the
        // length / envelope clocks, in dot units for the waveform).
        uint32_t waveform_cycles = 0;
        uint8_t  waveform_phase  = 0;       // 0..7 within the duty pattern
        uint32_t envelope_cycles = 0;
        uint32_t length_cycles   = 0;
    };
    Sound2 ch2_{};

    // ── Master state ──────────────────────────────────────────────
    bool    master_enable_ = false;  // SOUNDCNT_X bit 7
    uint8_t volume_l_ = 7;           // SOUNDCNT_L master volume left (0..7)
    uint8_t volume_r_ = 7;           // SOUNDCNT_L master volume right (0..7)
    bool    ch1_left_enable_  = false;
    bool    ch1_right_enable_ = false;
    bool    ch2_left_enable_  = false;
    bool    ch2_right_enable_ = false;
    // SOUNDCNT_H (0x082..0x083) bits 0..1: DMG output ratio.
    //   0=25% 1=50% 2=100% 3=prohibited (treated as 0).
    uint8_t dmg_volume_ratio_ = 1;

    struct DirectFifo {
        uint32_t words[8] = {};
        uint8_t write = 0;
        uint8_t read = 0;
        uint8_t count = 0;
        uint32_t shift_word = 0;
        uint8_t bytes_remaining = 0;
        int8_t samples[kMaxSamplesPerEvent] = {};
    };

    DirectFifo fifo_a_{};
    DirectFifo fifo_b_{};
    bool direct_a_left_ = false;
    bool direct_a_right_ = false;
    bool direct_a_timer1_ = false;
    bool direct_a_full_volume_ = false;
    bool direct_b_left_ = false;
    bool direct_b_right_ = false;
    bool direct_b_timer1_ = false;
    bool direct_b_full_volume_ = false;
    uint16_t soundbias_ = 0x0200;
    uint32_t cycles_per_sample_ = kDefaultCyclesPerSample;

    // Accumulates toward mGBA's 1024-cycle audio event. Each event
    // posts 2, 4, 8, or 16 samples depending on SOUNDBIAS resolution.
    uint32_t cycle_accumulator_ = 0;

    // Output ring. Mono int16_t samples at kSampleRate. The host
    // audio backend pulls from this in chunks.
    std::vector<int16_t> ring_;
    std::size_t ring_head_ = 0;
    std::size_t ring_tail_ = 0;
    uint64_t    samples_generated_ = 0;
    int16_t current_samples_[kMaxSamplesPerEvent] = {};
    uint32_t sample_index_ = 0;
    FifoTrace trace_[kFifoTraceSize] = {};
    uint32_t trace_write_ = 0;
    uint32_t trace_count_ = 0;

    void ring_push(int16_t s);
    void fifo_push_word(DirectFifo& fifo, uint32_t word);
    void fifo_clear_queue(DirectFifo& fifo);
    void fifo_reset(DirectFifo& fifo);
    void fifo_timer_step(DirectFifo& fifo, int fifo_id);
    void update_soundbias(uint16_t value);
    uint32_t samples_per_event() const;
    uint32_t cycles_until_next_sample_event() const;
    void sample_until_current_time();
    void run_sample_event();
    void ch1_trigger();
    void ch2_trigger();
    int16_t mix_one_sample(uint32_t direct_slot);
};

}  // namespace gba
