// gba_gpio.h — cartridge GPIO port at 0x080000C4..0x080000C9.
//
// The port is four pins (bits 0..3) plus a direction register and a read-enable
// control bit. Carts hang real devices off those pins: the Seiko S-3511A RTC
// (Pokémon Ruby/Sapphire/Emerald, Boktai, …), Boktai's solar sensor, gyro and
// rumble on other titles.
//
// Ownership split, mirroring the hardware:
//
//   * GpioPort  owns the three registers and the direction semantics. A pin the
//     guest configured as an OUTPUT reads back the level the guest wrote; a pin
//     configured as an INPUT reads whatever device is driving it (0 if none).
//   * GpioDevice is one chip on the wires. Every attached device sees every
//     data-port write and decides for itself whether it is being addressed —
//     there is no external mux. That is how a Boktai cart can carry both the
//     RTC and the solar sensor: they arbitrate on pin 2 (the RTC's CS), so
//     exactly one of them is live at a time.
//
// Reference for the multi-device behaviour: mGBA's src/gba/cart/gpio.c runs
// every attached device's pin handler on each write and merges their outputs
// through the direction mask. Hardware behaviour only — no code is derived from
// it (mGBA is MPL-2.0; see third_party/README.md, which requires the native
// build to carry no copyleft emulator dependencies).

#pragma once

#include <cstdint>

namespace gba {

class GpioDevice {
public:
    virtual ~GpioDevice() = default;

    // Has this device been detected on the current cartridge? Inactive devices
    // are skipped entirely, so a cart without the chip behaves as if the port
    // were plain ROM.
    virtual bool gpio_active() const = 0;

    // The guest wrote `pins` (low nibble) to the data register. Devices that
    // share the port must check their own select condition here.
    virtual void gpio_write(uint8_t pins) = 0;

    // Level this device drives on `bit` while the guest has that pin configured
    // as an input. Return -1 when this device does not drive the pin.
    virtual int gpio_drive(uint8_t bit) const = 0;
};

class GpioPort {
public:
    static constexpr uint32_t kData      = 0xC4;
    static constexpr uint32_t kDirection = 0xC6;
    static constexpr uint32_t kControl   = 0xC8;

    // Lowest / highest ROM-relative offsets the port answers, inclusive. The
    // high byte of each 16-bit register reads 0, hence the 0xC9 top.
    static constexpr uint32_t kFirst = 0xC4;
    static constexpr uint32_t kLast  = 0xC9;

    static bool in_range(uint32_t off) { return off >= kFirst && off <= kLast; }

    // Attaching twice is a no-op, so callers may re-run cartridge setup.
    void attach(GpioDevice* dev);

    // True when at least one attached device is present on this cartridge.
    bool active() const;

    // GPIO reads only answer while the guest has enabled read-back via the
    // control register; otherwise the bus falls through to ordinary ROM.
    bool read_enabled() const { return read_enable_; }

    uint8_t read(uint32_t off) const;
    void    write(uint32_t off, uint8_t value);

private:
    uint8_t read_data() const;
    // Sample every device-driven pin and hold the result. A pin latches the
    // level its device asserted at the moment the port was last written; it
    // does NOT re-ask the device on each read. That distinction matters for
    // Boktai: the solar sensor stops driving its output as soon as the RTC's
    // chip-select goes high, but the level it last drove must persist for the
    // game to read back.
    void latch_driven();

    uint8_t data_        = 0;   // low nibble significant
    uint8_t driven_      = 0;   // last levels asserted by devices
    uint8_t direction_   = 0;   // 1 = guest drives the pin
    bool    read_enable_ = false;

    static constexpr int kMaxDevices = 4;
    GpioDevice* devices_[kMaxDevices] = {};
    int         device_count_         = 0;
};

}  // namespace gba
