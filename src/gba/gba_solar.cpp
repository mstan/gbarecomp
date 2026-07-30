// gba_solar.cpp — see gba_solar.h.

#include "gba_solar.h"

#include <cstdio>
#include <cstdlib>

namespace gba {

void GbaSolarSensor::configure(bool enable) {
    active_ = enable && (std::getenv("RECOMP_SOLAR_OFF") == nullptr);
    counter_ = 0;
    threshold_ = 0xFF;
    prev_clock_low_ = false;
    selected_ = false;
    have_fixed_ = false;
    fixed_ = 0;

    if (const char* e = std::getenv("RECOMP_SOLAR_FIXED")) {
        char* end = nullptr;
        const long v = std::strtol(e, &end, 0);
        if (end != e && v >= 0 && v <= 255) {
            have_fixed_ = true;
            fixed_ = static_cast<uint8_t>(v);
        }
    }
    if (active_) {
        std::printf("solar_sensor=ENABLED%s\n",
                    have_fixed_ ? " (RECOMP_SOLAR_FIXED pinned)" : "");
    }
}

uint8_t GbaSolarSensor::sample_threshold() {
    if (have_fixed_)  return brightness_to_threshold(fixed_);
    if (provider_)    return brightness_to_threshold(provider_());
    // No host light source attached: darkest possible, comparator never trips.
    return 0xFF;
}

void GbaSolarSensor::gpio_write(uint8_t pins) {
    // Pin 2 is the RTC's chip select. While it is asserted the clock owns the
    // port and this device must not touch the wires at all.
    selected_ = (pins & kPinSelect) == 0;
    if (!selected_) return;

    // Reset: zero the counter and latch exactly one light sample, which is the
    // threshold this conversion compares against.
    if (pins & kPinReset) {
        counter_ = 0;
        // Preset "previous clock was low" so the first rising edge after a
        // reset counts. (mGBA marks level-vs-edge triggering here as
        // unverified; we match its level-triggered behaviour. If readings look
        // sticky or quantised, this is the first thing to vary.)
        prev_clock_low_ = true;
        threshold_ = sample_threshold();
    }

    // ADC clock: advance on the rising edge.
    const bool clock_high = (pins & kPinClock) != 0;
    if (clock_high && prev_clock_low_) {
        counter_ = static_cast<uint16_t>((counter_ + 1u) & kCounterMask);
    }
    prev_clock_low_ = !clock_high;
}

int GbaSolarSensor::gpio_drive(uint8_t bit) const {
    // Only the comparator pin, and only while the RTC is not holding the bus.
    if (bit != kPinOut || !selected_) return -1;
    return comparator() ? 1 : 0;
}

}  // namespace gba
