#pragma once

#include "gba_serial_device.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace gba {

// Four is the physical cable limit, NOT a session or wireless-player limit.
// The owner must stop execution at each endpoint's next event and rendezvous
// all machines before starting a transfer. No host clock or network API here.
class GbaLinkHub {
public:
    static constexpr std::size_t kCablePorts = 4;
    explicit GbaLinkHub(std::size_t port_count);
    ~GbaLinkHub();
    GbaLinkHub(const GbaLinkHub&) = delete;
    GbaLinkHub& operator=(const GbaLinkHub&) = delete;

    GbaSerialDevice& endpoint(std::size_t port);
    std::size_t port_count() const { return count_; }
    std::uint64_t cycle(std::size_t port) const;
    bool synchronized() const;
    bool transfer_active() const { return active_; }
    std::uint64_t completion_cycle() const { return finish_; }
    std::vector<std::uint8_t> save_state() const;
    // Transactional: malformed or differently wired state leaves hub intact.
    bool load_state(std::span<const std::uint8_t> bytes, std::string* error);

private:
    class Endpoint final : public GbaSerialDevice {
    public:
        Endpoint(GbaLinkHub& hub, std::size_t port) : hub_(hub), port_(port) {}
        std::uint16_t read16(std::uint32_t offset) const override;
        void write16(std::uint32_t offset, std::uint16_t value,
                     std::uint16_t byte_mask) override;
        void tick(std::uint32_t cycles) override;
        std::uint32_t cycles_until_event() const override;
        void complete();
    protected:
        void connection_changed() override;
    private:
        GbaLinkHub& hub_;
        std::size_t port_;
    };
    struct Port {
        std::uint64_t cycle = 0;
        std::uint16_t control = 0;
        std::uint16_t rcnt = 0x8000;
        std::uint16_t send = 0xffff;
        std::array<std::uint16_t, 4> receive{0xffff, 0xffff, 0xffff, 0xffff};
    };
    bool multiplayer(std::size_t port) const;
    bool normal(std::size_t port) const;
    bool ready() const;
    void start();
    void start_normal(std::size_t port);
    void cancel();
    void complete_if_ready();
    std::size_t count_;
    std::array<Port, kCablePorts> ports_{};
    std::array<std::unique_ptr<Endpoint>, kCablePorts> endpoints_{};
    std::array<std::uint16_t, kCablePorts> transfer_{0xffff, 0xffff, 0xffff, 0xffff};
    std::array<std::uint32_t, kCablePorts> normal_data_{};
    std::uint8_t normal_bits_ = 0, normal_mask_ = 0;
    std::uint64_t start_ = 0;
    bool active_ = false;
    bool destroying_ = false;
    std::uint64_t finish_ = 0;
};
} // namespace gba
