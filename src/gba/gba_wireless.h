#pragma once

#include "gba_serial_device.h"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace gba {

// A local RFU radio domain, independent of controller seats and cable ports.
// Each adapter has one serial connection; each radio parent has four child
// slots. More than one group can exist in the domain. No network/host clocks.
class GbaWirelessDomain {
public:
    static constexpr unsigned kChildSlots = 4;
    explicit GbaWirelessDomain(std::size_t adapters);
    ~GbaWirelessDomain();
    GbaWirelessDomain(const GbaWirelessDomain&) = delete;
    GbaWirelessDomain& operator=(const GbaWirelessDomain&) = delete;

    GbaSerialDevice& endpoint(std::size_t adapter);
    std::size_t adapter_count() const { return ports_.size(); }
    std::uint64_t cycle(std::size_t adapter) const;
    std::vector<std::uint8_t> save_state() const;
    bool load_state(std::span<const std::uint8_t>, std::string* error);

private:
    enum class Exchange : std::uint8_t { Identity, Command, Payload, Reply, Sleeping };
    struct Port {
        std::uint64_t cycle=0, transfer_at=0, radio_at=0, wait_until=0, advertised_at=0;
        std::uint32_t data=0, outgoing=0, setup=0;
        std::uint16_t control=0, rcnt=0x8000, last_identity=0, radio_id=0;
        std::uint8_t identity_step=0, command=0;
        std::uint16_t count=0, cursor=0;
        Exchange exchange=Exchange::Identity;
        bool identity_started=false, shifting=false, waiting=false, notice_ready=false, notice_ack=false;
        bool hosting=false, accepting=false, searching=false, advertised=false, tx_pending=false;
        std::uint8_t notice=0, max_children=4, slot=0xff;
        std::int32_t parent=-1;
        std::array<std::int32_t, kChildSlots> children{-1,-1,-1,-1};
        std::array<std::uint32_t,6> broadcast{};
        std::array<std::uint32_t,255> words{};
        std::array<std::uint8_t,87> tx{}, parent_rx{};
        std::array<std::array<std::uint8_t,16>,kChildSlots> child_rx{};
        std::uint8_t tx_size=0, parent_rx_size=0;
        std::array<std::uint8_t,kChildSlots> child_rx_size{};
    };
    class Endpoint final : public GbaSerialDevice {
    public:
        Endpoint(GbaWirelessDomain& domain, std::size_t index) : domain_(domain), index_(index) {}
        std::uint16_t read16(std::uint32_t) const override;
        void write16(std::uint32_t, std::uint16_t, std::uint16_t) override;
        void tick(std::uint32_t) override;
        std::uint32_t cycles_until_event() const override;
        void irq() { request_irq(); }
    private:
        void connection_changed() override;
        GbaWirelessDomain& domain_;
        std::size_t index_;
    };
    void reset(std::size_t);
    void disconnect(std::size_t, unsigned mask);
    void pump();
    void complete(std::size_t);
    std::uint32_t exchange(std::size_t, std::uint32_t outgoing);
    void command(std::size_t);
    void transmit(std::size_t);
    void notify(std::size_t, std::uint8_t command);
    void arm(std::size_t);
    unsigned free_slot(std::size_t) const;
    std::uint64_t next_event() const;
    template<class Archive, class State> static void state(Archive&, State&);
    std::vector<Port> ports_;
    std::vector<std::unique_ptr<Endpoint>> endpoints_;
};
} // namespace gba
