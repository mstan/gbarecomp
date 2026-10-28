// Native MusicPlayer2000 mono PCM service. This is a replacement for the
// reviewed SoundMainRAM operation, not a presentation shadow mixer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace gba {

struct Mp2kNativeMemory {
    std::span<const uint8_t> rom;
    std::span<uint8_t> ewram;
    std::span<uint8_t> iwram;
    const uint8_t* read(uint32_t address, std::size_t bytes) const;
    uint8_t* write(uint32_t address, std::size_t bytes) const;
};

struct Mp2kNativeCall {
    uint32_t sound_info = 0;
    uint32_t pcm_buffer = 0;
    uint32_t samples = 0;
    uint32_t dma_counter = 0;
};

struct Mp2kNativeStats {
    uint32_t channels = 0;
    uint32_t mixed_channels = 0;
    uint32_t direct_samples = 0;
    uint32_t interpolated_samples = 0;
    uint32_t source_advances = 0;
    uint32_t loop_wraps = 0;
};

enum class Mp2kNativeResult {
    Complete,
    UnsupportedInput,
};

// Reviewed mono driver with 0x40-byte channels at +0x50 and the 0x630-byte
// PCM ring at +0x350 (MKSC USA SoundMainRAM). Integer arithmetic reproduces
// the driver's PCM bytes and persistent channel/envelope state. All inputs
// are validated before mutation; unsupported inputs leave memory unchanged.
//
// This memory-only operation does not advance devices or restore the guest
// call ABI. Runtime selection/timing/continuation belong to its adapter and
// must be qualified before selecting this service in a game build. Borrowed
// views last for this invocation only; no pointers or state survive restore.
Mp2kNativeResult mp2k_native_mix(const Mp2kNativeMemory& memory,
                               const Mp2kNativeCall& call,
                               Mp2kNativeStats* stats = nullptr);

} // namespace gba
