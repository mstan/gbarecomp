#include "gba_wireless.h"
#include "simulation_archive.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace gba {
namespace {
constexpr std::uint64_t never=std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t frame_cycles=280896;
constexpr std::uint64_t radio_cycles=4096; // deterministic, provisional RF service interval
constexpr std::uint64_t discovery_retention=3*16777216ull;
constexpr std::uint32_t token=0x80000000, prefix=0x99660000;
constexpr std::array<std::uint16_t,5> identity{0x494e,0x544e,0x4e45,0x4f44,0x8001};
bool normal32(std::uint16_t control,std::uint16_t rcnt) {
    return !(rcnt&0x8000) && (control&0x3000)==0x1000;
}
}
GbaWirelessDomain::GbaWirelessDomain(std::size_t count) {
    // Allocation/snapshot safety bound, not an RFU population limit.
    if (!count || count>256) throw std::invalid_argument("invalid wireless domain size");
    ports_.resize(count);
    for (std::size_t i=0;i<count;++i) {
        ports_[i].radio_id=static_cast<std::uint16_t>(0x6000+i);
        endpoints_.push_back(std::make_unique<Endpoint>(*this,i));
    }
}
GbaWirelessDomain::~GbaWirelessDomain() = default;
void GbaWirelessDomain::Endpoint::connection_changed() {
    if (!connected()) domain_.reset(index_);
}
GbaSerialDevice& GbaWirelessDomain::endpoint(std::size_t i) { return *endpoints_.at(i); }
std::uint64_t GbaWirelessDomain::cycle(std::size_t i) const { return ports_.at(i).cycle; }

std::uint16_t GbaWirelessDomain::Endpoint::read16(std::uint32_t offset) const {
    const auto& p=domain_.ports_[index_];
    switch (offset) {
    case 0x120: return static_cast<std::uint16_t>(p.data);
    case 0x122: return static_cast<std::uint16_t>(p.data>>16);
    case 0x128: {
        // RFU acknowledgement follows SO, with reversed polarity while the
        // GBA owns the clock. BUSY is the serial shifter, not radio activity.
        bool si=(p.control&8)!=0;
        if (p.control&1) si=!si;
        return static_cast<std::uint16_t>((p.control&~4u)|(si ? 4:0));
    }
    case 0x134: return p.rcnt;
    default: return 0;
    }
}
void GbaWirelessDomain::Endpoint::write16(std::uint32_t offset,std::uint16_t value,std::uint16_t mask) {
    auto& p=domain_.ports_[index_];
    const auto merge=[&](std::uint16_t previous) { return static_cast<std::uint16_t>((previous&~mask)|(value&mask)); };
    if (offset==0x120 || offset==0x122) {
        const unsigned shift=(offset-0x120)*8;
        p.data=(p.data&~(std::uint32_t(mask)<<shift))|(std::uint32_t(value&mask)<<shift);
    } else if (offset==0x134) {
        const auto next=merge(p.rcnt);
        if ((next&0x8022)==0x8022 && !(p.rcnt&2)) domain_.reset(index_);
        p.rcnt=next;
        if (next&0x8000) { p.shifting=false; p.control&=~0x80; }
    } else if (offset==0x128) {
        const auto old=p.control;
        p.control=merge(old)&0x7ffb; // SI is peripheral-owned
        if (!normal32(p.control,p.rcnt)) { p.shifting=false; p.control&=~0x80; return; }
        if (!(p.control&0x80)) p.shifting=false;
        // Identity probing can arm an external transfer, then select the
        // internal clock without clearing START first.
        if ((p.control&0x80) && (!p.shifting || !(old&1))) domain_.arm(index_);
    }
}
void GbaWirelessDomain::arm(std::size_t i) {
    auto& p=ports_[i];
    if (!(p.control&0x80) || p.shifting || !normal32(p.control,p.rcnt)) return;
    if (!(p.control&1) && (!p.waiting || !p.notice_ready)) return;
    p.outgoing=p.data;
    p.transfer_at=p.cycle+((p.control&2) ? 256:2048);
    p.shifting=true;
}
void GbaWirelessDomain::Endpoint::tick(std::uint32_t cycles) {
    auto& p=domain_.ports_[index_];
    if (p.cycle>never-cycles) throw std::overflow_error("RFU cycle overflow");
    p.cycle+=cycles; domain_.pump();
}
std::uint64_t GbaWirelessDomain::next_event() const {
    auto next=never;
    for (std::size_t i=0;i<ports_.size();++i) {
        if (!endpoints_[i]->connected()) continue;
        const auto& p=ports_[i];
        if (p.shifting) next=std::min(next,p.transfer_at);
        if (p.radio_at) next=std::min(next,p.radio_at);
        if (p.waiting && !p.notice_ready && p.wait_until) next=std::min(next,p.wait_until);
    }
    return next;
}
std::uint32_t GbaWirelessDomain::Endpoint::cycles_until_event() const {
    const auto now=domain_.ports_[index_].cycle, next=domain_.next_event();
    if (next<=now) return 0;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(next-now,0xffffffffu));
}
void GbaWirelessDomain::pump() {
    auto now=never;
    for (std::size_t i=0;i<ports_.size();++i)
        if (endpoints_[i]->connected()) now=std::min(now,ports_[i].cycle);
    if (now==never) return;
    for (std::size_t i=0;i<ports_.size();++i) {
        if (!endpoints_[i]->connected()) continue;
        auto& p=ports_[i];
        if (p.shifting && p.transfer_at<=now) complete(i);
        if (p.radio_at && p.radio_at<=now) { p.radio_at=0; transmit(i); }
        if (p.waiting && !p.notice_ready && p.wait_until && p.wait_until<=now) notify(i,0x27);
    }
}
void GbaWirelessDomain::complete(std::size_t i) {
    auto& p=ports_[i];
    p.data=exchange(i,p.outgoing);
    p.shifting=false; p.control&=~0x80;
    if (p.control&0x4000) endpoints_[i]->irq();
}
unsigned GbaWirelessDomain::free_slot(std::size_t i) const {
    const auto& p=ports_[i];
    if (p.accepting)
        for (unsigned slot=0;slot<p.max_children;++slot) if (p.children[slot]<0) return slot;
    return 0xff;
}
void GbaWirelessDomain::notify(std::size_t i,std::uint8_t command) {
    auto& p=ports_[i];
    if (!p.notice_ready) { p.notice=command; p.notice_ready=true; p.notice_ack=false; }
    arm(i);
}
void GbaWirelessDomain::disconnect(std::size_t i,unsigned mask) {
    auto& p=ports_[i];
    if (p.parent>=0 && (mask&(1u<<p.slot))) {
        ports_[p.parent].children[p.slot]=-1;
        p.parent=-1; p.slot=0xff; p.tx_pending=false;
    }
    for (unsigned slot=0;slot<4;++slot) if ((mask&(1u<<slot)) && p.children[slot]>=0) {
        const auto child=static_cast<std::size_t>(p.children[slot]);
        auto& c=ports_[child]; c.parent=-1; c.slot=0xff; c.tx_pending=false;
        p.children[slot]=-1; p.child_rx_size[slot]=0;
        notify(child,0x29);
    }
}
void GbaWirelessDomain::reset(std::size_t i) {
    disconnect(i,15);
    const auto old=ports_[i];
    ports_[i]=Port{};
    ports_[i].cycle=old.cycle; ports_[i].radio_id=old.radio_id;
}
std::uint32_t GbaWirelessDomain::exchange(std::size_t i,std::uint32_t sent) {
    auto& p=ports_[i];
    if (p.waiting && !(p.control&1)) {
        if (!p.notice_ack) { p.notice_ack=true; return prefix|p.notice; }
        if (sent==(prefix|(p.notice+0x80))) {
            p.waiting=false; p.notice_ready=false; p.notice_ack=false; p.wait_until=0;
            p.exchange=Exchange::Command;
        }
        return token;
    }
    if (p.exchange==Exchange::Identity) {
        const auto word=static_cast<std::uint16_t>(sent);
        auto found=std::find(identity.begin(),identity.end(),word);
        const auto reply=!p.identity_started || found==identity.end() ? 0u : (std::uint32_t(word)<<16)|std::uint16_t(~p.last_identity);
        p.identity_started=true;
        if (found!=identity.end()) {
            const auto step=static_cast<unsigned>(found-identity.begin());
            if (step==0) p.identity_step=1;
            else if (step<=p.identity_step && static_cast<std::uint16_t>(sent>>16)==static_cast<std::uint16_t>(~p.last_identity)) {
                p.identity_step=static_cast<std::uint8_t>(step+1);
                if (step==4) p.exchange=Exchange::Command;
            }
        }
        p.last_identity=word;
        return reply;
    }
    if (p.exchange==Exchange::Command) {
        if ((sent&0xffff0000)!=prefix) return token;
        p.command=static_cast<std::uint8_t>(sent); p.count=(sent>>8)&255; p.cursor=0;
        p.exchange=Exchange::Payload;
        if (!p.count) command(i);
        return token;
    }
    if (p.exchange==Exchange::Payload) {
        p.words[p.cursor++]=sent;
        if (p.cursor==p.count) command(i);
        return token;
    }
    if (p.exchange==Exchange::Reply) {
        const auto result=p.cursor ? p.words[p.cursor-1] : prefix|(p.count<<8)|p.command;
        if (++p.cursor>p.count) {
            p.exchange=Exchange::Command;
            if (p.command==0xbd) p.exchange=Exchange::Sleeping;
            if (p.command==0xa5 || p.command==0xa7 || p.command==0xb7) {
                p.waiting=true;
                p.wait_until=(p.setup&255) ? p.cycle+(p.setup&255)*frame_cycles:0;
            }
        }
        return result;
    }
    return token;
}
void GbaWirelessDomain::command(std::size_t i) {
    auto& p=ports_[i];
    const auto cmd=p.command;
    std::vector<std::uint32_t> reply;
    unsigned error=0;
    const auto length=[&](unsigned n) { if (p.count!=n) error=2; return !error; };
    const auto child_list=[&] {
        for (unsigned slot=0;slot<4;++slot)
            if (p.children[slot]>=0) reply.push_back(ports_[p.children[slot]].radio_id|(slot<<16));
    };
    switch (cmd) {
    case 0x10:
        if (length(0)) { disconnect(i,15); p.hosting=p.accepting=p.searching=p.advertised=false; }
        break;
    case 0x11:
        if (length(0)) {
            std::uint32_t strength=0;
            for (unsigned slot=0;slot<4;++slot)
                if (p.children[slot]>=0 || (p.parent>=0 && p.slot==slot)) strength|=255u<<(slot*8);
            reply.push_back(strength);
        }
        break;
    case 0x12: if (length(0)) reply.push_back(8585495); break;
    case 0x13:
        if (length(0)) {
            unsigned mode=p.parent>=0 ? 5 : p.searching ? 3 : p.hosting ? (p.accepting ? 2:1):0;
            reply.push_back((mode ? p.radio_id:0)|(p.parent>=0 ? (1u<<p.slot)<<16:0)|(mode<<24));
        }
        break;
    case 0x14: if (length(0)) { reply.push_back(free_slot(i)); child_list(); } break;
    case 0x15:
        if (length(0)) { reply.assign(p.broadcast.begin(),p.broadcast.end()); reply.push_back(p.setup); reply.push_back(257); }
        break;
    case 0x16:
        if (length(6)) {
            std::copy_n(p.words.begin(),6,p.broadcast.begin());
            if (p.accepting) { p.advertised=true; p.advertised_at=p.cycle; }
        }
        break;
    case 0x17:
        if (length(1)) { p.setup=p.words[0]; p.max_children=static_cast<std::uint8_t>(4-((p.setup>>16)&3)); }
        break;
    case 0x19:
        if (length(0)) {
            if (p.parent>=0) error=1;
            else { p.hosting=p.accepting=p.advertised=true; p.searching=false; p.advertised_at=p.cycle; }
        }
        break;
    case 0x1a: case 0x1b:
        if (length(0)) {
            if (!p.hosting || !p.accepting) error=1;
            else { child_list(); if (cmd==0x1b) p.accepting=false; }
        }
        break;
    case 0x1c:
        if (length(0)) { p.searching=true; p.accepting=false; }
        break;
    case 0x1d: case 0x1e:
        if (length(0)) {
            if (!p.searching) error=1;
            else {
                for (std::size_t peer=0;peer<ports_.size() && reply.size()<28;++peer) {
                    const auto& host=ports_[peer];
                    if (peer==i || !endpoints_[peer]->connected() || !host.advertised ||
                        (!host.accepting && p.cycle-host.advertised_at>discovery_retention)) continue;
                    reply.push_back(host.radio_id|(free_slot(peer)<<16));
                    reply.insert(reply.end(),host.broadcast.begin(),host.broadcast.end());
                }
                if (cmd==0x1e) p.searching=false;
            }
        }
        break;
    case 0x1f:
        if (length(1)) {
            if (p.parent>=0) { error=1; break; }
            for (std::size_t peer=0;peer<ports_.size();++peer) {
                if (peer==i || !endpoints_[peer]->connected() || ports_[peer].radio_id!=static_cast<std::uint16_t>(p.words[0])) continue;
                const auto slot=free_slot(peer);
                if (slot==255) break;
                p.parent=static_cast<std::int32_t>(peer); p.slot=static_cast<std::uint8_t>(slot);
                p.hosting=p.accepting=p.searching=false; ports_[peer].children[slot]=static_cast<std::int32_t>(i);
                break;
            }
        }
        break;
    case 0x20: case 0x21:
        if (length(0)) reply.push_back(p.parent>=0 ? p.radio_id|(unsigned(p.slot)<<16) : 0x01000000);
        break;
    case 0x24: case 0x25:
        if (!p.count || (!p.hosting && p.parent<0)) { error=1; break; }
        {
            const unsigned size=p.parent>=0 ? (p.words[0]>>(8+5*p.slot))&31 : p.words[0]&127;
            if (size>(p.parent>=0 ? 16u:87u) || (p.count>1 && (p.count-1)*4<size)) { error=2; break; }
            // A header-only send retransmits the retained bytes.
            if (p.count>1) for (unsigned byte=0;byte<size;++byte) p.tx[byte]=static_cast<std::uint8_t>(p.words[1+byte/4]>>(8*(byte%4)));
            p.tx_size=static_cast<std::uint8_t>(size); p.tx_pending=true;
            if (p.hosting) p.radio_at=p.cycle+radio_cycles;
        }
        break;
    case 0x26:
        if (length(0)) {
            std::vector<std::uint8_t> payload;
            std::uint32_t sizes=0;
            if (p.parent>=0) {
                sizes=p.parent_rx_size; payload.assign(p.parent_rx.begin(),p.parent_rx.begin()+p.parent_rx_size); p.parent_rx_size=0;
            } else {
                for (unsigned slot=0;slot<4;++slot) {
                    sizes|=std::uint32_t(p.child_rx_size[slot])<<(8+5*slot);
                    payload.insert(payload.end(),p.child_rx[slot].begin(),p.child_rx[slot].begin()+p.child_rx_size[slot]);
                    p.child_rx_size[slot]=0;
                }
            }
            if (sizes) {
                reply.resize(1+(payload.size()+3)/4); reply[0]=sizes;
                for (unsigned byte=0;byte<payload.size();++byte) reply[1+byte/4]|=std::uint32_t(payload[byte])<<(8*(byte%4));
            }
        }
        break;
    case 0x27: length(0); break;
    case 0x30: if (length(1)) disconnect(i,p.words[0]&15); break;
    case 0x37: if (length(0)) { if (!p.hosting) error=1; else p.radio_at=p.cycle+radio_cycles; } break;
    case 0x3d:
        if (length(0)) {
            disconnect(i,15); p.hosting=p.accepting=p.searching=p.advertised=false;
            p.waiting=p.notice_ready=p.notice_ack=p.tx_pending=false;
            p.radio_at=p.wait_until=0;
        }
        break;
    default: error=2; break;
    }
    if (std::getenv("GBA_RFU_TRACE"))
        std::fprintf(stderr,"rfu adapter=%zu cycle=%llu cmd=%02x words=%u reply=%zu error=%u parent=%d slot=%u\n",
            i,static_cast<unsigned long long>(p.cycle),cmd,p.count,reply.size(),error,p.parent,p.slot);
    p.command=static_cast<std::uint8_t>(error ? 0xee : cmd|0x80);
    if (error) reply={error};
    std::fill(p.words.begin(),p.words.end(),0);
    std::copy(reply.begin(),reply.end(),p.words.begin());
    p.count=static_cast<std::uint16_t>(reply.size()); p.cursor=0; p.exchange=Exchange::Reply;
}
void GbaWirelessDomain::transmit(std::size_t i) {
    auto& p=ports_[i];
    if (!p.hosting) return;
    for (unsigned slot=0;slot<4;++slot) if (p.children[slot]>=0) {
        const auto child=static_cast<std::size_t>(p.children[slot]); auto& c=ports_[child];
        if (p.tx_size) { std::copy_n(p.tx.begin(),p.tx_size,c.parent_rx.begin()); c.parent_rx_size=p.tx_size; notify(child,0x28); }
        if (c.tx_pending) {
            std::copy_n(c.tx.begin(),c.tx_size,p.child_rx[slot].begin()); p.child_rx_size[slot]=c.tx_size; c.tx_pending=false;
        }
    }
    p.tx_pending=false; notify(i,0x28);
}

template<class Archive,class State> void GbaWirelessDomain::state(Archive& a,State& p) {
    a(p.cycle,p.transfer_at,p.radio_at,p.wait_until,p.advertised_at,p.data,p.outgoing,p.setup,
      p.control,p.rcnt,p.last_identity,p.radio_id,p.identity_step,p.command,p.count,p.cursor,p.exchange,
      p.identity_started,p.shifting,p.waiting,p.notice_ready,p.notice_ack,p.hosting,p.accepting,p.searching,p.advertised,p.tx_pending,
      p.notice,p.max_children,p.slot,p.parent,p.children,p.broadcast,p.words,p.tx,p.parent_rx,p.child_rx,
      p.tx_size,p.parent_rx_size,p.child_rx_size);
}
std::vector<std::uint8_t> GbaWirelessDomain::save_state() const {
    SimulationArchive<false> a;
    a.identity(std::uint32_t{0x55465247}); a.identity(std::uint32_t{1});
    a.identity(static_cast<std::uint32_t>(ports_.size()));
    for (const auto& p:ports_) state(a,p);
    return a.take();
}
bool GbaWirelessDomain::load_state(std::span<const std::uint8_t> bytes,std::string* error) {
    try {
        SimulationArchive<true> a(bytes);
        a.identity(std::uint32_t{0x55465247}); a.identity(std::uint32_t{1});
        a.identity(static_cast<std::uint32_t>(ports_.size()));
        auto staged=ports_;
        for (auto& p:staged) {
            state(a,p);
            if (p.identity_step>5 || p.count>255 || p.cursor>256 || p.cursor>p.count+1 ||
                p.exchange>Exchange::Sleeping || p.max_children<1 || p.max_children>4 ||
                (p.exchange==Exchange::Reply && p.cursor>p.count) ||
                (p.exchange==Exchange::Payload && p.cursor>=p.count) ||
                p.parent< -1 || p.parent>=static_cast<std::int32_t>(ports_.size()) ||
                (p.parent<0 ? p.slot!=255 : p.slot>=4) || p.tx_size>87 || p.parent_rx_size>87 ||
                (p.parent>=0 && p.tx_size>16) || (p.shifting && (!(p.control&0x80) || p.transfer_at<p.cycle)) ||
                (p.radio_at && p.radio_at<p.cycle) || (p.accepting && !p.hosting))
                throw std::invalid_argument("invalid wireless adapter state");
            for (unsigned slot=0;slot<4;++slot)
                if (p.children[slot]< -1 || p.children[slot]>=static_cast<std::int32_t>(ports_.size()) || p.child_rx_size[slot]>16)
                    throw std::invalid_argument("invalid RFU child slot");
        }
        for (std::size_t i=0;i<staged.size();++i) {
            const auto& p=staged[i];
            if (p.radio_id!=0x6000+i || (p.parent>=0 &&
                (p.parent==static_cast<std::int32_t>(i) || staged[p.parent].children[p.slot]!=static_cast<std::int32_t>(i))))
                throw std::invalid_argument("inconsistent RFU association");
            for (unsigned slot=0;slot<4;++slot) if (p.children[slot]>=0) {
                const auto& c=staged[p.children[slot]];
                if (c.parent!=static_cast<std::int32_t>(i) || c.slot!=slot || !p.hosting)
                    throw std::invalid_argument("inconsistent RFU child association");
            }
        }
        if (a.remaining()) throw std::invalid_argument("trailing RFU state data");
        ports_=std::move(staged); return true;
    } catch (const std::exception& e) { if (error) *error=e.what(); return false; }
}
} // namespace gba
