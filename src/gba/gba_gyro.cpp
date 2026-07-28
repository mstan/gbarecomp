// gba_gyro.cpp — see gba_gyro.h.
//
// Wire behavior is cross-checked against mGBA's independently implemented
// cartridge GPIO model (src/gba/cart/gpio.c): sample on pin 0, shift MSB-first
// on pin-1 falling edges, data on pin 2, rumble on pin 3.

#include "gba_gyro.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace gba {
namespace {

bool is_warioware_twisted(const uint8_t* rom, std::size_t len) {
    if (!rom || len < 0xB0) return false;
    const char* code = reinterpret_cast<const char*>(rom + 0xAC);
    return std::memcmp(code, "RZWE", 4) == 0 ||
           std::memcmp(code, "RZWJ", 4) == 0 ||
           std::memcmp(code, "RZWP", 4) == 0;
}

}  // namespace

void GbaGyro::configure(const uint8_t* rom, std::size_t len) {
    active_ = is_warioware_twisted(rom, len);
    clock_high_ = false;
    output_valid_ = false;
    output_level_ = false;
    rumble_on_ = false;
    sample_offset_ = 0;
    shift_ = 0;
    sample_sequence_ = 0;
    test_sweep_ = false;

    if (!active_) return;
    if (const char* value = std::getenv("GBARECOMP_GYRO_TEST")) {
        if (std::strcmp(value, "sweep") == 0) {
            test_sweep_ = true;
        } else if (*value) {
            set_sample_offset(static_cast<int>(std::strtol(value, nullptr, 0)));
        }
    }
}

void GbaGyro::set_sample_offset(int offset) {
    sample_offset_ = std::clamp(offset, -0x700, 0x8FF);
}

void GbaGyro::capture_sample() {
    int offset = sample_offset_;
    if (test_sweep_) {
        // Deterministic triangle wave spanning roughly +/-0x500. Advancing per
        // cartridge sample keeps headless tests independent of host frame rate.
        constexpr int kHalfPeriod = 120;
        const int phase = static_cast<int>(sample_sequence_++ %
                                           (kHalfPeriod * 2));
        const int ramp = phase < kHalfPeriod
            ? phase
            : (kHalfPeriod * 2 - 1 - phase);
        offset = -0x500 + (ramp * 0xA00) / (kHalfPeriod - 1);
    }
    shift_ = static_cast<uint16_t>(0x700 + offset);
}

void GbaGyro::gpio_write(uint8_t pins) {
    pins &= 0x0F;

    // The serial converter presents the next bit on a falling clock edge.
    bool output = clock_high_ && !(pins & 0x02);
    if (pins & 0x01) {
        capture_sample();
        output = true;
    }

    if (output) {
        output_level_ = (shift_ >> 15) != 0;
        output_valid_ = true;
        shift_ = static_cast<uint16_t>(shift_ << 1);
    }

    clock_high_ = (pins & 0x02) != 0;
    rumble_on_ = (pins & 0x08) != 0;
}

int GbaGyro::gpio_drive(uint8_t bit) const {
    if (bit == 2 && output_valid_) return output_level_ ? 1 : 0;
    return -1;
}

}  // namespace gba

