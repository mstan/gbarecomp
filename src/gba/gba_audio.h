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
    static constexpr uint32_t kSampleRate     = 32768u;
    static constexpr uint32_t kCyclesPerSample = kSystemHz / kSampleRate;  // 512

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
    // accumulator crosses kCyclesPerSample.
    void tick(uint32_t cycles);

    // Drain up to `max` samples from the ring into `out`. Returns
    // the number actually written. Used by the host audio backend
    // and by the TCP audio_samples command.
    std::size_t drain_samples(int16_t* out, std::size_t max);

    // Total samples generated since reset — useful for sync diff.
    uint64_t samples_generated() const { return samples_generated_; }

private:
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
    uint8_t volume_l_ = 7;           // SOUNDCNT_L master volume left
    uint8_t volume_r_ = 7;           // SOUNDCNT_L master volume right
    bool    ch2_left_enable_  = false;
    bool    ch2_right_enable_ = false;

    // Sample-generation accumulator (in system cycles).
    uint32_t cycle_accumulator_ = 0;

    // Output ring. Mono int16_t samples at kSampleRate. The host
    // audio backend pulls from this in chunks.
    std::vector<int16_t> ring_;
    std::size_t ring_head_ = 0;
    std::size_t ring_tail_ = 0;
    uint64_t    samples_generated_ = 0;

    void ring_push(int16_t s);
    void ch2_trigger();
    int16_t mix_one_sample();
};

}  // namespace gba
