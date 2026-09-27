#pragma once

#include <cstdint>

namespace gba {
class GbaIo;

// A device on ONE machine's serial port. Cable and (future) RFU adapters
// implement this interface; a wireless medium is not a larger cable hub.
// Wiring is host state. Device state belongs in the enclosing session snapshot.
class GbaSerialDevice {
public:
    virtual ~GbaSerialDevice();
    GbaSerialDevice() = default;
    GbaSerialDevice(const GbaSerialDevice&) = delete;
    GbaSerialDevice& operator=(const GbaSerialDevice&) = delete;

    static bool handles(std::uint32_t offset) {
        return (offset >= 0x120 && offset <= 0x12b) ||
               (offset >= 0x134 && offset <= 0x135);
    }
    virtual std::uint16_t read16(std::uint32_t offset) const = 0;
    // byte_mask preserves hardware-owned bits on byte writes without a
    // read/modify/write through a register with read side effects.
    virtual void write16(std::uint32_t offset, std::uint16_t value,
                         std::uint16_t byte_mask = 0xffff) = 0;
    virtual void tick(std::uint32_t cycles) = 0;
    virtual std::uint32_t cycles_until_event() const = 0;
    bool connected() const { return io_ != nullptr; }

protected:
    void request_irq();
    virtual void connection_changed() {}

private:
    friend class GbaIo;
    GbaIo* io_ = nullptr;
};
} // namespace gba
