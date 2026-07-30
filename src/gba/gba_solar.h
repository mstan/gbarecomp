// gba_solar.h — Boktai cartridge solar sensor (photodiode + integrating ADC).
//
// Device emulation, not HLE: the guest's own driver bit-bangs the conversion
// and counts clocks, exactly as it would against the real chip. Used by the
// Boktai / Bokura no Taiyou line, which carries this sensor AND the S-3511A RTC
// on the same four GPIO pins.
//
// ── Pin map (bits of the GPIO data port at 0x080000C4) ───────────────
//   0  ADC clock  — the counter advances on the rising edge
//   1  reset      — zeroes the counter and latches one light sample
//   2  chip select (the RTC's CS) — while HIGH the clock owns the bus and this
//      device stays off the wires entirely. This is how the two chips share
//      the port: exactly one is live at a time.
//   3  comparator output — driven high once counter >= threshold
//
// ── Protocol ─────────────────────────────────────────────────────────
// It reports light as a *time*, not a value. The guest resets, then pulses the
// clock and counts how many pulses pass before pin 3 flips. Fewer clocks means
// more light.
//
// ── The value is inverted ────────────────────────────────────────────
// Because the latched sample is a *threshold*, a SMALL threshold trips the
// comparator SOONER. So 0x00 is the brightest reading and 0xFF the darkest; an
// unlit / absent sensor sits at 0xFF and effectively never trips. Providers
// speak in ordinary brightness (0 = dark, 255 = bright) and the conversion is
// funnelled through one named function so the polarity cannot be flipped by
// accident — getting it backwards would make sunlight read as midnight.
//
// ── Sourcing ─────────────────────────────────────────────────────────
// Pin assignment and comparator behaviour were read from mGBA's
// src/gba/cart/gpio.c (_lightReadPins) as a HARDWARE REFERENCE only; no code is
// derived from it. mGBA is MPL-2.0 and third_party/README.md requires the
// native build to carry no copyleft emulator dependencies, while
// docs/ARCHITECTURE.md explicitly permits borrowing "hardware reference
// behavior from emulators and hardware docs".

#pragma once

#include <cstdint>

#include "gba_gpio.h"

namespace gba {

class GbaSolarSensor final : public GpioDevice {
public:
    // Returns ordinary brightness: 0 = pitch dark, 255 = full sun.
    using LightProvider = uint8_t (*)();

    // The sensor cannot be detected from the ROM the way the RTC can (there is
    // no library signature for it), so it is an explicit opt-in per game.
    // Honors RECOMP_SOLAR_OFF, and RECOMP_SOLAR_FIXED=<0-255> pins a constant
    // brightness for deterministic runs — the same shape as RECOMP_RTC_EPOCH.
    void configure(bool enable);

    bool active() const { return active_; }

    // Host light source. Null means "no sensor illuminated": the threshold
    // stays at its darkest and the comparator never trips.
    void set_provider(LightProvider p) { provider_ = p; }

    // ── GpioDevice ───────────────────────────────────────────────────
    bool gpio_active() const override { return active_; }
    void gpio_write(uint8_t pins) override;
    int  gpio_drive(uint8_t bit) const override;

    // Introspection for tests and the on-screen light meter.
    uint16_t counter() const { return counter_; }
    uint8_t  threshold() const { return threshold_; }
    bool     comparator() const { return counter_ >= threshold_; }

    // The single place the polarity flip lives.
    static uint8_t brightness_to_threshold(uint8_t brightness) {
        return static_cast<uint8_t>(255u - brightness);
    }

private:
    uint8_t sample_threshold();

    static constexpr uint8_t kPinClock  = 0x1;
    static constexpr uint8_t kPinReset  = 0x2;
    static constexpr uint8_t kPinSelect = 0x4;   // RTC chip select
    static constexpr uint8_t kPinOut    = 3;     // bit index
    static constexpr uint16_t kCounterMask = 0x0FFF;   // 12-bit counter

    bool          active_   = false;
    LightProvider provider_ = nullptr;

    uint16_t counter_   = 0;
    uint8_t  threshold_ = 0xFF;   // darkest until a sample is latched
    // Tracks whether the previous clock level was LOW, so a rising edge can be
    // detected. Reset presets it true, matching the reference implementation.
    bool prev_clock_low_ = false;
    // The sensor only drives its output pin while selected (CS low).
    bool selected_ = false;

    bool    have_fixed_ = false;   // RECOMP_SOLAR_FIXED override
    uint8_t fixed_      = 0;
};

}  // namespace gba
