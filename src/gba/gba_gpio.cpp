// gba_gpio.cpp — see gba_gpio.h.

#include "gba_gpio.h"

namespace gba {

void GpioPort::attach(GpioDevice* dev) {
    if (!dev || device_count_ >= kMaxDevices) return;
    for (int i = 0; i < device_count_; ++i) {
        if (devices_[i] == dev) return;   // idempotent
    }
    devices_[device_count_++] = dev;
}

bool GpioPort::active() const {
    for (int i = 0; i < device_count_; ++i) {
        if (devices_[i]->gpio_active()) return true;
    }
    return false;
}

void GpioPort::latch_driven() {
    uint8_t lv = 0;
    for (uint8_t bit = 0; bit < 4; ++bit) {
        if (direction_ & (1u << bit)) continue;   // guest drives this pin
        for (int i = 0; i < device_count_; ++i) {
            if (!devices_[i]->gpio_active()) continue;
            const int d = devices_[i]->gpio_drive(bit);
            if (d >= 0) {                          // first claimant wins
                lv = static_cast<uint8_t>(lv | ((d & 1) << bit));
                break;
            }
        }
    }
    driven_ = lv;
}

uint8_t GpioPort::read_data() const {
    uint8_t v = 0;
    for (uint8_t bit = 0; bit < 4; ++bit) {
        const uint8_t level = (direction_ & (1u << bit))
            ? static_cast<uint8_t>((data_ >> bit) & 1u)     // guest's own level
            : static_cast<uint8_t>((driven_ >> bit) & 1u);  // last device level
        v = static_cast<uint8_t>(v | (level << bit));
    }
    return v;
}

uint8_t GpioPort::read(uint32_t off) const {
    if (!read_enable_) return 0;
    switch (off) {
        case kData:      return read_data();
        case kDirection: return direction_;
        case kControl:   return 1;
        default:         return 0;  // high byte of each halfword register
    }
}

void GpioPort::write(uint32_t off, uint8_t value) {
    switch (off) {
        case kData:
            data_ = static_cast<uint8_t>(value & 0x0F);
            // Every attached device sees every write and self-selects.
            for (int i = 0; i < device_count_; ++i) {
                if (devices_[i]->gpio_active()) devices_[i]->gpio_write(data_);
            }
            latch_driven();
            break;
        case kDirection:
            direction_ = static_cast<uint8_t>(value & 0x0F);
            latch_driven();   // a pin that just became an input needs a level
            break;
        case kControl:
            read_enable_ = (value & 1) != 0;
            break;
        default:
            break;
    }
}

}  // namespace gba
