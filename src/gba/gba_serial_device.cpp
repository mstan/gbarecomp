#include "gba_serial_device.h"
#include "gba_io.h"
#include <stdexcept>

namespace gba {
GbaSerialDevice::~GbaSerialDevice() {
    if (io_) io_->set_serial_device(nullptr);
}

void GbaSerialDevice::request_irq() {
    if (io_) io_->request_irq(1u << 7);
}

void GbaIo::set_serial_device(GbaSerialDevice* device) {
    if (device == serial_device_) return;
    if (device && device->io_)
        throw std::logic_error("serial device already belongs to a GBA");
    if (serial_device_) {
        auto* old = serial_device_;
        serial_device_ = nullptr;
        old->io_ = nullptr;
        old->connection_changed();
    }
    // A transfer from the old disconnected-port model cannot leak into a
    // newly attached device (or resume when that device is removed).
    sio_transfer_active_ = false;
    sio_cycles_remaining_ = 0;
    serial_device_ = device;
    if (device) {
        device->io_ = this;
        device->connection_changed();
    }
}
} // namespace gba
