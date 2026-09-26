#include "gba_link_hub.h"
#include "snapshot.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <cstdio>

namespace gba {
namespace {
constexpr std::uint16_t kBusy = 0x80, kIrq = 0x4000;
// Behavioral timing observations, in 16.777216 MHz CPU cycles. Rows are
// 9600/38400/57600/115200 baud, columns total cable participants 2/3/4.
// See docs/MULTIPLAYER.md for oracle provenance and qualification status.
constexpr std::uint32_t kTransferCycles[4][3] = {
    {63427, 94884, 125829}, {16241, 24104, 31457},
    {10998, 16241, 20972}, {5755, 8376, 10486}
};
}

GbaLinkHub::GbaLinkHub(std::size_t port_count) : count_(port_count) {
    if (count_ < 2 || count_ > kCablePorts)
        throw std::invalid_argument("GBA cable requires two to four ports");
    for (std::size_t i = 0; i < count_; ++i)
        endpoints_[i] = std::make_unique<Endpoint>(*this, i);
}
GbaLinkHub::~GbaLinkHub() { destroying_ = true; }
GbaSerialDevice& GbaLinkHub::endpoint(std::size_t port) {
    if (port >= count_) throw std::out_of_range("GBA cable port");
    return *endpoints_[port];
}
std::uint64_t GbaLinkHub::cycle(std::size_t port) const {
    if (port >= count_) throw std::out_of_range("GBA cable port");
    return ports_[port].cycle;
}
bool GbaLinkHub::synchronized() const {
    for (std::size_t i = 1; i < count_; ++i)
        if (ports_[i].cycle != ports_[0].cycle) return false;
    return true;
}
bool GbaLinkHub::multiplayer(std::size_t port) const {
    const auto& p = ports_[port];
    return !(p.rcnt & 0x8000) && (p.control & 0x3000) == 0x2000;
}
bool GbaLinkHub::normal(std::size_t port) const {
    const auto& p=ports_[port];
    return !(p.rcnt&0x8000) && !(p.control&0x2000);
}
bool GbaLinkHub::ready() const {
    for (std::size_t i = 0; i < count_; ++i)
        if (!endpoints_[i]->connected() || !multiplayer(i)) return false;
    return true;
}
std::uint16_t GbaLinkHub::Endpoint::read16(std::uint32_t off) const {
    const auto& p = hub_.ports_[port_];
    if (off >= 0x120 && off <= 0x126 && !(off & 1))
        return p.receive[(off - 0x120) / 2];
    if (off == 0x12a) return p.send;
    if (off == 0x134) {
        if (!hub_.multiplayer(port_)) return p.rcnt;
        return static_cast<std::uint16_t>((p.rcnt & 0xc1f0) |
            (hub_.active_ ? 0 : 1) | (hub_.ready() ? 2 : 0) |
            (port_ ? 4 : 0));
    }
    if (off == 0x128) {
        if (hub_.normal(port_)) {
            // The multiplayer cable routes SO forward along the chain, not
            // back to the primary. Normal-mode primary SI floats high.
            const bool si=port_==0 || !hub_.endpoints_[port_-1]->connected() ||
                (hub_.ports_[port_-1].control&8);
            return static_cast<std::uint16_t>((p.control&0x508b)|(si ? 4 : 0));
        }
        if (!hub_.multiplayer(port_)) return p.control;
        return static_cast<std::uint16_t>((p.control & 0x7003) |
            (port_ ? 4 : 0) | (hub_.ready() ? 8 : 0) |
            (port_ << 4) | (hub_.active_ && !hub_.normal_bits_ ? kBusy : 0));
    }
    return 0;
}
void GbaLinkHub::Endpoint::write16(std::uint32_t off, std::uint16_t value,
                                  std::uint16_t mask) {
    auto& p = hub_.ports_[port_];
    if (off == 0x12a) {
        p.send = static_cast<std::uint16_t>((p.send & ~mask) | (value & mask));
    } else if ((off==0x120 || off==0x122) && hub_.normal(port_) && (p.control&0x1000)) {
        auto& data=p.receive[(off-0x120)/2];
        data=static_cast<std::uint16_t>((data&~mask)|(value&mask));
    } else if (off == 0x134 || off == 0x128) {
        auto& reg = off == 0x134 ? p.rcnt : p.control;
        const auto old = reg;
        reg = static_cast<std::uint16_t>((reg & ~mask) | (value & mask));
        const bool mode_changed=off==0x134 ? ((old^reg)&0xc000) : ((old^reg)&0x3000);
        if (mode_changed || (off==0x128 && hub_.normal_bits_ &&
            (hub_.normal_mask_&(1u<<port_)) && !(reg&kBusy))) {
            const auto written=reg;
            hub_.cancel();
            reg=written;
        }
        // Normal external-clock mode can be armed indefinitely: no clock
        // means no shifted bits and no completion IRQ. The real BIOS does
        // this during its cartridge/multiboot probe. Actual normal-clock
        // transfers are locally timed just like multiplayer transfers.
        if (off==0x128 && hub_.normal(port_) && (mask&value&kBusy)) {
            if (p.control&1) hub_.start_normal(port_);
        } else if (off == 0x128 && !(p.rcnt & 0x8000) && !hub_.multiplayer(port_) &&
            (mask & value & kBusy)) {
            char error[100];
            std::snprintf(error,sizeof(error),"unqualified serial start: SIOCNT=%04x RCNT=%04x",p.control,p.rcnt);
            throw std::logic_error(error);
        }
        if (off == 0x128 && hub_.multiplayer(port_)) {
            reg &= 0x7003;
            if (port_ == 0 && (mask & value & kBusy) && !hub_.active_) {
                if (!hub_.synchronized()) {
                    reg = old;
                    throw std::logic_error("SIO start requires a session rendezvous");
                }
                hub_.start();
            }
        }
    }
    // SIOMULTI receive registers are hardware-owned in multiplayer mode.
}
void GbaLinkHub::start_normal(std::size_t port) {
    if (!synchronized()) throw std::logic_error("normal serial start requires a session rendezvous");
    // This multiplayer-cable backend has one clock owner, port zero. Other
    // ports may arm a transfer (including the RFU detection run by each
    // cartridge at boot), but only the primary drives the shared clock.
    // Independent normal-mode clock ownership needs a different cable policy.
    if (port!=0) return;
    const auto bits=(ports_[port].control&0x1000) ? 32u : 8u;
    const auto duration=bits*((ports_[port].control&2) ? 8u : 64u);
    if (active_) {
        if (normal_bits_ && (normal_mask_&(1u<<port))) return; // BUSY held high is not another edge
        // Reconfiguration during an active incompatible transfer is invalid.
        if (!normal_bits_ || ports_[port].cycle!=start_ || normal_bits_!=bits || finish_-start_!=duration)
            throw std::logic_error("conflicting serial clock drivers");
        normal_mask_|=static_cast<std::uint8_t>(1u<<port);
        return;
    }
    if (ports_[port].cycle>std::numeric_limits<std::uint64_t>::max()-duration)
        throw std::overflow_error("normal serial cycle overflow");
    normal_bits_=static_cast<std::uint8_t>(bits); normal_mask_=0;
    for (std::size_t i=0;i<count_;++i) {
        const auto& p=ports_[i];
        if (endpoints_[i]->connected() && normal(i) && ((p.control&0x1000)!=0)==(bits==32)) {
            normal_data_[i]=bits==8 ? p.send&0xff : p.receive[0]|(std::uint32_t(p.receive[1])<<16);
            if (p.control&kBusy) normal_mask_|=static_cast<std::uint8_t>(1u<<i);
        } else normal_data_[i]=0xffffffff;
    }
    start_=ports_[port].cycle; finish_=start_+duration; active_=true;
}
void GbaLinkHub::start() {
    if (!ready()) return;
    const auto duration = kTransferCycles[ports_[0].control & 3][count_ - 2];
    if (ports_[0].cycle > std::numeric_limits<std::uint64_t>::max() - duration)
        throw std::overflow_error("GBA cable cycle overflow");
    transfer_.fill(0xffff);
    for (std::size_t i = 0; i < count_; ++i) {
        transfer_[i] = ports_[i].send;
        ports_[i].receive.fill(0xffff);
    }
    finish_ = ports_[0].cycle + duration;
    active_ = true;
}
void GbaLinkHub::cancel() {
    if (normal_bits_) for (std::size_t i=0;i<count_;++i)
        if (normal_mask_&(1u<<i)) ports_[i].control&=~kBusy;
    active_ = false;
    normal_bits_=normal_mask_=0; start_=0; normal_data_.fill(0);
    finish_ = 0;
    transfer_.fill(0xffff);
}
void GbaLinkHub::Endpoint::tick(std::uint32_t cycles) {
    auto& p = hub_.ports_[port_];
    if (cycles > std::numeric_limits<std::uint64_t>::max() - p.cycle)
        throw std::overflow_error("GBA serial cycle overflow");
    if (hub_.active_ && p.cycle + cycles > hub_.finish_)
        throw std::logic_error("GBA crossed an unprocessed cable event");
    p.cycle += cycles;
    hub_.complete_if_ready();
}
std::uint32_t GbaLinkHub::Endpoint::cycles_until_event() const {
    if (!hub_.active_) return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(hub_.finish_ - hub_.ports_[port_].cycle);
}
void GbaLinkHub::Endpoint::complete() {
    if (hub_.ports_[port_].control & kIrq) request_irq();
}
void GbaLinkHub::complete_if_ready() {
    if (!active_) return;
    for (std::size_t i = 0; i < count_; ++i)
        if (ports_[i].cycle != finish_) return;
    active_ = false;
    if (normal_bits_) {
        for (std::size_t i=0;i<count_;++i) if (normal_mask_&(1u<<i)) {
            const auto data=i && (normal_mask_&(1u<<(i-1))) ? normal_data_[i-1] : 0xffffffff;
            if (normal_bits_==8) ports_[i].send=(ports_[i].send&0xff00)|(data&0xff);
            else { ports_[i].receive[0]=data&0xffff; ports_[i].receive[1]=data>>16; }
            ports_[i].control&=~kBusy;
            endpoints_[i]->complete();
        }
    } else {
        for (std::size_t i = 0; i < count_; ++i) ports_[i].receive = transfer_;
        for (std::size_t i = 0; i < count_; ++i) endpoints_[i]->complete();
    }
    normal_bits_=normal_mask_=0; start_=0; normal_data_.fill(0);
    finish_ = 0;
    transfer_.fill(0xffff);
}
void GbaLinkHub::Endpoint::connection_changed() {
    if (!hub_.destroying_) hub_.cancel();
}
std::vector<std::uint8_t> GbaLinkHub::save_state() const {
    gbarecomp::debug::SnapshotWriter w;
    w.u32(0x4b4e4c47); // GLNK
    w.u32(2);
    w.u32(static_cast<std::uint32_t>(count_));
    w.boolean(active_);
    w.u64(finish_);
    w.u8(normal_bits_); w.u8(normal_mask_); w.u64(start_);
    for (auto data:normal_data_) w.u32(data);
    for (auto word : transfer_) w.u16(word);
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& p = ports_[i];
        w.boolean(endpoints_[i]->connected());
        w.u64(p.cycle); w.u16(p.control); w.u16(p.rcnt); w.u16(p.send);
        for (auto word : p.receive) w.u16(word);
    }
    return w.take_buffer();
}
bool GbaLinkHub::load_state(std::span<const std::uint8_t> bytes, std::string* error) {
    auto fail = [&] { if (error) *error = "invalid cable snapshot or wiring mismatch"; return false; };
    constexpr std::size_t header = 55, per_port = 23;
    if (bytes.size() != header + count_ * per_port) return fail();
    gbarecomp::debug::SnapshotReader r(bytes.data(), bytes.size());
    if (r.u32() != 0x4b4e4c47 || r.u32() != 2 || r.u32() != count_) return fail();
    auto active = r.u8();
    auto finish = r.u64();
    auto normal_bits=r.u8(), normal_mask=r.u8(); const auto start=r.u64();
    std::array<std::uint32_t,4> normal_data;
    for (auto& data:normal_data) data=r.u32();
    if (normal_bits!=0 && normal_bits!=8 && normal_bits!=32) return fail();
    if (normal_bits && (!active || !normal_mask || normal_mask>=(1u<<count_) || start>=finish ||
        (finish-start!=normal_bits*8u && finish-start!=normal_bits*64u))) return fail();
    if (!normal_bits && (normal_mask || start || normal_data!=std::array<std::uint32_t,4>{})) return fail();
    std::array<std::uint16_t, 4> transfer;
    for (auto& word : transfer) word = r.u16();
    auto ports = ports_;
    bool all_finished = true;
    for (std::size_t i = 0; i < count_; ++i) {
        if (r.u8() != static_cast<unsigned>(endpoints_[i]->connected())) return fail();
        auto& p = ports[i];
        p.cycle = r.u64(); p.control = r.u16(); p.rcnt = r.u16(); p.send = r.u16();
        for (auto& word : p.receive) word = r.u16();
        if (active && p.cycle>finish) return fail();
        if (active && !normal_bits && (!endpoints_[i]->connected() ||
                       (p.rcnt & 0x8000) || (p.control & 0x3000) != 0x2000)) return fail();
        if (normal_bits && (normal_mask&(1u<<i)) && (!endpoints_[i]->connected() ||
            p.cycle<start || (p.rcnt&0x8000) || (p.control&0x2000) || !(p.control&kBusy) ||
            ((p.control&0x1000)!=0)!=(normal_bits==32))) return fail();
        all_finished &= p.cycle == finish;
    }
    if (!r.ok() || r.remaining() || active > 1 ||
        (active && (!finish || all_finished)) || (!active && finish)) return fail();
    ports_ = ports; transfer_ = transfer; active_ = active != 0; finish_ = finish;
    normal_bits_=normal_bits; normal_mask_=normal_mask; normal_data_=normal_data; start_=start;
    return true;
}
} // namespace gba
