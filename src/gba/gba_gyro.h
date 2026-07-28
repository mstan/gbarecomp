// gba_gyro.h — WarioWare: Twisted! cartridge gyroscope + rumble lines.
//
// The cartridge exposes a serial gyroscope on GPIO pin 2, sampled by pin 0
// and clocked by pin 1. Pin 3 controls the rumble motor. Host input is reduced
// to a signed sample offset here; SDL, Android, and deterministic test sources
// remain outside the wire-protocol model.

#pragma once

#include <cstddef>
#include <cstdint>

#include "gba_gpio.h"

namespace gba {

class GbaGyro final : public GpioDevice {
public:
    void configure(const uint8_t* rom, std::size_t len);

    bool gpio_active() const override { return active_; }
    void gpio_write(uint8_t pins) override;
    int gpio_drive(uint8_t bit) const override;

    bool active() const { return active_; }
    bool rumble_on() const { return rumble_on_; }

    // Host-neutral angular-rate input. Zero is the calibrated center; values
    // are clamped to the cartridge's useful 12-bit range around 0x700.
    void set_sample_offset(int offset);
    int sample_offset() const { return sample_offset_; }

private:
    void capture_sample();

    bool active_ = false;
    bool clock_high_ = false;
    bool output_valid_ = false;
    bool output_level_ = false;
    bool rumble_on_ = false;
    bool test_sweep_ = false;
    int sample_offset_ = 0;
    uint16_t shift_ = 0;
    uint32_t sample_sequence_ = 0;
};

}  // namespace gba

