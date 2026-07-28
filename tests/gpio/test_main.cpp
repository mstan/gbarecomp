// gpio_tests — cartridge GPIO port (0x080000C4..0xC9) and its device model.
//
// Covers the direction semantics and the multi-device arbitration that the
// port must get right before a second device (Boktai's solar sensor) is hung
// off the same four pins alongside the RTC.

#include <cstdint>
#include <cstdio>

#include "gba_gpio.h"

namespace {

int failures = 0;

void check_eq(const char* test, const char* tag, unsigned got, unsigned expect) {
    if (got != expect) {
        std::printf("FAIL %s: %s mismatch (got 0x%X, expected 0x%X)\n",
                    test, tag, got, expect);
        ++failures;
    }
}

void check_true(const char* test, const char* tag, bool cond) {
    if (!cond) {
        std::printf("FAIL %s: %s\n", test, tag);
        ++failures;
    }
}

// A device that drives one pin with a settable level and records writes.
class FakeDevice final : public gba::GpioDevice {
public:
    FakeDevice(int pin, bool active) : pin_(pin), active_(active) {}

    bool gpio_active() const override { return active_; }
    void gpio_write(uint8_t pins) override { last_pins_ = pins; ++writes_; }
    int gpio_drive(uint8_t bit) const override {
        if (!claims_) return -1;
        return (static_cast<int>(bit) == pin_) ? (level_ ? 1 : 0) : -1;
    }

    void set_level(bool v) { level_ = v; }
    void set_claims(bool v) { claims_ = v; }
    uint8_t last_pins() const { return last_pins_; }
    int writes() const { return writes_; }

private:
    int  pin_;
    bool active_;
    bool level_ = false;
    bool claims_ = true;
    uint8_t last_pins_ = 0xFF;
    int writes_ = 0;
};

// Reads only answer once the control register has enabled read-back.
void test_read_enable() {
    const char* T = "read_enable";
    gba::GpioPort port;
    FakeDevice dev(1, true);
    port.attach(&dev);

    port.write(gba::GpioPort::kDirection, 0x0F);
    port.write(gba::GpioPort::kData, 0x0A);
    check_eq(T, "reads are 0 before control enables them",
             port.read(gba::GpioPort::kData), 0);
    check_true(T, "read_enabled() false initially", !port.read_enabled());

    port.write(gba::GpioPort::kControl, 1);
    check_true(T, "read_enabled() true after control write", port.read_enabled());
    check_eq(T, "data reads back once enabled",
             port.read(gba::GpioPort::kData), 0x0A);
    check_eq(T, "direction register reads back",
             port.read(gba::GpioPort::kDirection), 0x0F);
    check_eq(T, "control register reads 1",
             port.read(gba::GpioPort::kControl), 1);
    check_eq(T, "high byte of a halfword register reads 0",
             port.read(0xC5), 0);
}

// A pin configured as an OUTPUT reads back the guest's own written level, even
// when a device would otherwise drive it. An INPUT pin reads the device.
void test_direction_semantics() {
    const char* T = "direction";
    gba::GpioPort port;
    FakeDevice dev(1, true);
    port.attach(&dev);
    port.write(gba::GpioPort::kControl, 1);

    dev.set_level(true);

    // All pins guest-driven: the device must not influence the read.
    port.write(gba::GpioPort::kDirection, 0x0F);
    port.write(gba::GpioPort::kData, 0x00);
    check_eq(T, "output pins read back the written level (device ignored)",
             port.read(gba::GpioPort::kData), 0x00);

    // Pin 1 as an input: the device drives it high.
    port.write(gba::GpioPort::kDirection, 0x0D);   // bit1 clear = input
    port.write(gba::GpioPort::kData, 0x00);
    check_eq(T, "input pin reads the device level", port.read(gba::GpioPort::kData), 0x02);

    // Device levels are LATCHED at the port write, not re-queried per read
    // (see latch_driven()), so a level change only becomes visible after the
    // next port access.
    dev.set_level(false);
    port.write(gba::GpioPort::kData, 0x00);
    check_eq(T, "input pin follows the device low", port.read(gba::GpioPort::kData), 0x00);

    // An input pin no device claims reads 0.
    dev.set_claims(false);
    port.write(gba::GpioPort::kData, 0x00);
    check_eq(T, "unclaimed input pin reads 0", port.read(gba::GpioPort::kData), 0x00);

    // Only the low nibble is significant.
    port.write(gba::GpioPort::kDirection, 0xF0);
    check_eq(T, "direction masks to the low nibble",
             port.read(gba::GpioPort::kDirection), 0x00);
}

// Every attached device sees every write and self-selects; devices may claim
// different pins simultaneously. This is what lets Boktai's RTC and solar
// sensor share the port.
void test_multi_device() {
    const char* T = "multi_device";
    gba::GpioPort port;
    FakeDevice a(1, true);
    FakeDevice b(3, true);
    port.attach(&a);
    port.attach(&b);
    port.write(gba::GpioPort::kControl, 1);
    port.write(gba::GpioPort::kDirection, 0x00);   // all pins are inputs

    a.set_level(true);
    b.set_level(true);
    port.write(gba::GpioPort::kData, 0x05);

    check_eq(T, "both devices observed the write", a.last_pins(), 0x05);
    check_eq(T, "second device observed the write too", b.last_pins(), 0x05);
    check_eq(T, "both driven pins merge into one read",
             port.read(gba::GpioPort::kData), 0x0A);   // bits 1 and 3
}

// Inactive devices are skipped entirely, and a port with no active device
// reports inactive so the bus can fall through to ordinary ROM.
void test_active_gating() {
    const char* T = "active_gating";
    gba::GpioPort empty;
    check_true(T, "empty port is inactive", !empty.active());

    gba::GpioPort port;
    FakeDevice off(1, false);
    port.attach(&off);
    check_true(T, "port with only inactive devices is inactive", !port.active());

    port.write(gba::GpioPort::kControl, 1);
    port.write(gba::GpioPort::kDirection, 0x00);
    off.set_level(true);
    port.write(gba::GpioPort::kData, 0x0F);
    check_eq(T, "inactive device receives no writes", off.writes(), 0);
    check_eq(T, "inactive device drives nothing", port.read(gba::GpioPort::kData), 0x00);

    FakeDevice on(2, true);
    port.attach(&on);
    check_true(T, "port becomes active when a live device attaches", port.active());
}

void test_attach_is_idempotent() {
    const char* T = "attach_idempotent";
    gba::GpioPort port;
    FakeDevice dev(1, true);
    port.attach(&dev);
    port.attach(&dev);
    port.attach(&dev);
    port.write(gba::GpioPort::kControl, 1);
    port.write(gba::GpioPort::kData, 0x01);
    check_eq(T, "re-attaching the same device does not duplicate writes",
             dev.writes(), 1);
    port.attach(nullptr);   // must not crash
}

// Device-driven levels are sampled when the port is written and then held.
// This is what lets a device that has gone quiet (Boktai's solar sensor once
// the RTC asserts chip-select) leave its last output readable on the pin.
void test_driven_levels_are_latched() {
    const char* T = "latch";
    gba::GpioPort port;
    FakeDevice dev(1, true);
    port.attach(&dev);
    port.write(gba::GpioPort::kControl, 1);
    port.write(gba::GpioPort::kDirection, 0x00);   // every pin an input

    dev.set_level(true);
    port.write(gba::GpioPort::kData, 0x00);        // latches high
    check_eq(T, "latched high", port.read(gba::GpioPort::kData), 0x02);

    // The device goes quiet entirely; the latched level must survive.
    dev.set_claims(false);
    check_eq(T, "level persists after the device stops driving",
             port.read(gba::GpioPort::kData), 0x02);

    // A direction change re-latches, because a pin that just became an input
    // needs a level from somewhere.
    port.write(gba::GpioPort::kDirection, 0x00);
    check_eq(T, "direction write re-latches (device now silent)",
             port.read(gba::GpioPort::kData), 0x00);
}

void test_in_range() {
    const char* T = "in_range";
    check_true(T, "0xC4 in range", gba::GpioPort::in_range(0xC4));
    check_true(T, "0xC9 in range", gba::GpioPort::in_range(0xC9));
    check_true(T, "0xC3 out of range", !gba::GpioPort::in_range(0xC3));
    check_true(T, "0xCA out of range", !gba::GpioPort::in_range(0xCA));
}

}  // namespace

int main() {
    test_read_enable();
    test_direction_semantics();
    test_multi_device();
    test_active_gating();
    test_driven_levels_are_latched();
    test_attach_is_idempotent();
    test_in_range();

    if (failures) {
        std::printf("gpio_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("gpio_tests: all checks passed\n");
    return 0;
}
