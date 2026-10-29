#include "mp2k_native.h"

#include <array>
#include <bit>
#include <cstring>

namespace gba {
namespace {

constexpr uint32_t kMagic = 0x68736d53;
constexpr uint32_t kChannels = 0x50;
constexpr uint32_t kStride = 0x40;
constexpr uint32_t kPcm = 0x350;
constexpr uint32_t kPcmBytes = 0x630;

uint32_t load32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void store32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i != 4; ++i) p[i] = uint8_t(value >> (8 * i));
}
int32_t signed_byte(uint8_t value) { return std::bit_cast<int8_t>(value); }
int32_t arithmetic_shift(uint32_t bits, unsigned n) {
    return std::bit_cast<int32_t>(bits) >> n;
}
bool overlaps(const uint8_t* a, std::size_t a_bytes,
              const uint8_t* b, std::size_t b_bytes) {
    const auto left = reinterpret_cast<uintptr_t>(a);
    const auto right = reinterpret_cast<uintptr_t>(b);
    return left < right + b_bytes && right < left + a_bytes;
}

struct Channel {
    std::array<uint8_t, kStride> bytes{};
    const uint8_t* data = nullptr;
    const uint8_t* loop = nullptr;
    uint32_t data_address = 0;
    uint32_t remaining = 0;
    uint32_t loop_length = 0;
    uint32_t loop_address = 0;
    uint32_t fraction = 0;
    uint32_t step = 0;
    bool active = false;
    bool fixed_rate = false;
};

bool prepare_channel(const Mp2kNativeMemory& memory, const uint8_t* source,
                     uint32_t period, uint8_t master, const uint8_t* info,
                     Channel& channel) {
    std::memcpy(channel.bytes.data(), source, kStride);
    auto* c = channel.bytes.data();
    unsigned status = c[0];
    if ((status & 0xc7) == 0) return true;
    unsigned envelope = c[9];
    const uint32_t wave_address = load32(c + 0x24);
    // Planning all voices before committing is valid only when a preceding
    // voice cannot rewrite a later voice's wave header or sample data.
    if (status & 0x90) {
        const auto* wave = memory.read(wave_address, 16);
        if ((wave_address & 3) || !wave ||
            overlaps(wave, 16, info, kPcm + kPcmBytes)) return false;
    }
    if (status & 0x80) {
        if (status & 0x40) { c[0] = 0; return true; }
        const auto* wave = memory.read(wave_address, 16);
        if (!wave) return false;
        status = 3;
        store32(c + 0x28, wave_address + 16);
        store32(c + 0x18, load32(wave + 12));
        c[9] = 0;
        store32(c + 0x1c, 0);
        envelope = 0;
        if (wave[3] & 0xc0) status |= 0x10;
        c[0] = uint8_t(status);
        envelope += c[4];
        if (envelope >= 255) { envelope = 255; c[0] = uint8_t(--status); }
    } else if (status & 4) {
        const unsigned length = c[13];
        c[13] = uint8_t(length - 1);
        if (length <= 1) { c[0] = 0; return true; }
    } else {
        bool echo = false;
        if (status & 0x40) {
            envelope = (envelope * c[7]) >> 8;
            echo = envelope <= c[12];
        } else if ((status & 3) == 2) {
            envelope = (envelope * c[5]) >> 8;
            if (envelope <= c[6]) {
                envelope = c[6];
                if (envelope == 0) echo = true;
                else c[0] = uint8_t(--status);
            }
        } else if ((status & 3) == 3) {
            envelope += c[4];
            if (envelope >= 255) { envelope = 255; c[0] = uint8_t(--status); }
        }
        if (echo) {
            envelope = c[12];
            if (envelope == 0) { c[0] = 0; return true; }
            c[0] = uint8_t(status |= 4);
        }
    }
    c[9] = uint8_t(envelope);
    const unsigned volume = (envelope * (unsigned(master) + 1)) >> 4;
    c[10] = uint8_t((volume * (unsigned(c[2]) + c[3])) >> 9);
    if (status & 0x10) {
        const auto* wave = memory.read(wave_address, 16);
        if (!wave) return false;
        const uint32_t length = load32(wave + 12);
        const uint32_t start = load32(wave + 8);
        if (length <= start) return false;
        channel.loop_length = length - start;
        channel.loop_address = wave_address + 16 + start;
    }
    channel.data_address = load32(c + 0x28);
    channel.remaining = load32(c + 0x18);
    channel.fraction = load32(c + 0x1c);
    channel.step = period * load32(c + 0x20); // ARM MUL retains low 32 bits.
    channel.fixed_rate = (c[1] & 8) != 0;
    // The supported driver reads the next byte for linear interpolation.
    // A nonpositive count/corrupt pointer is outside this native contract.
    if (channel.remaining == 0 || channel.remaining > 0x01000000) return false;
    channel.data = memory.read(channel.data_address,
                               std::size_t(channel.remaining) + 1);
    if (!channel.data || overlaps(channel.data, std::size_t(channel.remaining) + 1,
                                  info, kPcm + kPcmBytes)) return false;
    if (channel.loop_length) {
        channel.loop = memory.read(channel.loop_address,
                                   std::size_t(channel.loop_length) + 1);
        if (!channel.loop || overlaps(channel.loop, std::size_t(channel.loop_length) + 1,
                                      info, kPcm + kPcmBytes)) return false;
    }
    // The reviewed PCM mixer has no compressed/reverse decode branch. Other
    // driver families must get their own qualified operation, not this one.
    channel.active = true;
    return true;
}

} // namespace

const uint8_t* Mp2kNativeMemory::read(uint32_t address, std::size_t bytes) const {
    std::span<const uint8_t> region;
    std::size_t offset;
    switch (address >> 24) {
    case 2: region = ewram; offset = address & 0x3ffff; break;
    case 3: region = iwram; offset = address & 0x7fff; break;
    case 8: case 9: case 10: case 11: case 12: case 13:
        region = rom; offset = address & 0x1ffffff; break;
    default: return nullptr;
    }
    if (offset > region.size() || bytes > region.size() - offset) return nullptr;
    return region.data() + offset;
}
uint8_t* Mp2kNativeMemory::write(uint32_t address, std::size_t bytes) const {
    if ((address >> 24) != 2 && (address >> 24) != 3) return nullptr;
    return const_cast<uint8_t*>(read(address, bytes));
}

Mp2kNativeResult mp2k_native_mix(const Mp2kNativeMemory& memory,
                               const Mp2kNativeCall& call,
                               Mp2kNativeStats* stats) {
    auto* info = memory.write(call.sound_info, kPcm + kPcmBytes);
    if (!info || ((call.sound_info | call.pcm_buffer) & 3) ||
        load32(info) != kMagic + 1 ||
        info[6] == 0 || info[6] > 12 || call.samples < 16 ||
        call.samples > kPcmBytes || (call.samples & 3) ||
        call.pcm_buffer < call.sound_info + kPcm ||
        uint64_t(call.pcm_buffer) + call.samples >
            uint64_t(call.sound_info) + kPcm + kPcmBytes)
        return Mp2kNativeResult::UnsupportedInput;

    auto* output = memory.write(call.pcm_buffer, call.samples);
    if (!output) return Mp2kNativeResult::UnsupportedInput;
    const uint8_t reverb = info[5];
    const uint32_t other_address = call.dma_counter == 2
        ? call.sound_info + kPcm : call.pcm_buffer + call.samples;
    const uint8_t* other = reverb ? memory.read(other_address, call.samples) : nullptr;
    if (reverb && !other) return Mp2kNativeResult::UnsupportedInput;

    const uint32_t period = load32(info + 0x18);
    std::array<Channel, 12> channels{};
    for (unsigned i = 0; i < info[6]; ++i) {
        if (!prepare_channel(memory, info + kChannels + i * kStride,
                             period, info[7], info, channels[i]))
            return Mp2kNativeResult::UnsupportedInput;
    }

    // A conservative instruction/data-wait bound for an atomic service window.
    // Source advances and short loops are included, so high pitches cannot
    // turn a small PCM block into unbounded SDK loop work. The reference
    // differential gate checks this against worst supported region waits.
    uint64_t bound = 200 + uint64_t(call.samples) * (reverb ? 40 : 4) +
        uint64_t(info[6]) * 240;
    for (unsigned i = 0; i < info[6]; ++i) {
        const auto& channel = channels[i];
        if (!channel.active) continue;
        bound += uint64_t(call.samples) * (channel.fixed_rate ? 40 : 64);
        if (!channel.fixed_rate) {
            const uint64_t advances = (uint64_t(channel.fraction) +
                uint64_t(channel.step) * call.samples) >> 23;
            bound += advances * 16;
            if (channel.loop_length) bound += (1 + advances / channel.loop_length) * 12;
        }
    }
    if (bound > call.max_guest_cycles) return Mp2kNativeResult::UnsupportedInput;
    Mp2kNativeStats work{};
    work.guest_cycle_upper_bound = uint32_t(bound);
    if (!reverb) std::memset(output, 0, call.samples);
    else for (uint32_t i = 0; i < call.samples; ++i) {
        int32_t value = ((signed_byte(output[i]) + signed_byte(other[i])) * reverb) >> 8;
        if (value & 0x80) ++value;
        output[i] = uint8_t(value);
    }

    for (unsigned i = 0; i < info[6]; ++i) {
        ++work.channels;
        auto& channel = channels[i];
        auto* destination = info + kChannels + i * kStride;
        const auto* c = channel.bytes.data();
        // Commit only the fields the guest operation can write. Inactive
        // channel payload and unrelated SDK links/callbacks remain untouched.
        destination[0] = c[0];
        destination[9] = c[9];
        destination[10] = c[10];
        destination[13] = c[13];
        store32(destination + 0x18, load32(c + 0x18));
        store32(destination + 0x1c, load32(c + 0x1c));
        store32(destination + 0x28, load32(c + 0x28));
        if (!channel.active) continue;
        ++work.mixed_channels;
        const uint8_t* source = channel.data;
        uint32_t source_address = channel.data_address;
        uint32_t remaining = channel.remaining;
        uint32_t fraction = channel.fraction;
        bool ended = false;
        int32_t first = signed_byte(source[0]);
        int32_t difference = signed_byte(source[1]) - first;
        for (uint32_t sample = 0; sample < call.samples; ++sample) {
            int32_t value;
            uint32_t advance;
            if (channel.fixed_rate) {
                value = signed_byte(*source);
                advance = 1;
                ++work.direct_samples;
            } else {
                value = first + arithmetic_shift(fraction * uint32_t(difference), 23);
                fraction += channel.step;
                advance = fraction >> 23;
                if (advance) fraction &= ~0x3f800000u;
                ++work.interpolated_samples;
            }
            const int32_t contribution = (value * c[10]) >> 8;
            output[sample] = uint8_t(uint32_t(output[sample]) + uint32_t(contribution));
            if (advance) {
                work.source_advances += advance;
                if (advance >= remaining) {
                    if (!channel.loop_length) {
                        destination[0] = 0;
                        ended = true;
                        break;
                    }
                    uint32_t overrun = advance - remaining;
                    work.loop_wraps += 1 + overrun / channel.loop_length;
                    overrun %= channel.loop_length;
                    remaining = channel.loop_length - overrun;
                    source = channel.loop + overrun;
                    source_address = channel.loop_address + overrun;
                } else {
                    remaining -= advance;
                    source += advance;
                    source_address += advance;
                }
                if (!channel.fixed_rate) {
                    first = signed_byte(source[0]);
                    difference = signed_byte(source[1]) - first;
                }
            }
        }
        // Completion of a non-looping voice clears status without publishing
        // its transient count/fraction/data registers (the original ABI).
        if (!ended) {
            store32(destination + 0x18, remaining);
            store32(destination + 0x28, source_address);
            if (!channel.fixed_rate) store32(destination + 0x1c, fraction);
        }
    }
    store32(info, kMagic);
    work.guest_cycle_estimate = 100 + call.samples * (reverb ? 20u : 2u) +
        work.channels * 60 + work.direct_samples * 12 +
        work.interpolated_samples * 24 + work.source_advances * 8 + work.loop_wraps * 12;
    if (stats) *stats = work;
    return Mp2kNativeResult::Complete;
}

} // namespace gba
