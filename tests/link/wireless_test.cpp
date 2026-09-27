#include "gba_wireless.h"
#include "gba_io.h"
#include <cstdio>
#include <cstdlib>
#include <memory>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while (0)
namespace {
struct Radio {
    std::vector<std::unique_ptr<gba::GbaIo>> io;
    gba::GbaWirelessDomain domain;
    explicit Radio(unsigned count=2) : domain(count) {
        for (unsigned i=0;i<count;++i) {
            io.push_back(std::make_unique<gba::GbaIo>());
            io.back()->set_serial_device(&domain.endpoint(i));
            io.back()->write16(0x134,0);
        }
    }
    void tick(unsigned cycles) { for (auto& p:io) p->tick_sio(cycles); }
    std::uint32_t shift(unsigned i,std::uint32_t word,bool fast=true,bool internal=true) {
        auto& port=*io[i];
        port.write32(0x120,word);
        const unsigned bits=0x5000|(fast?2:0)|(internal?1:0);
        port.write16(0x128,bits|0x80);
        const unsigned cycles=fast ? 256:2048;
        tick(cycles-1); CHECK(port.read16(0x128)&0x80);
        tick(1); CHECK(!(port.read16(0x128)&0x80));
        CHECK(port.read16(0x202)&0x80);
        const auto result=port.read32(0x120);
        CHECK(bool(port.read16(0x128)&4)==internal);
        port.write16(0x128,bits|8);
        CHECK(bool(port.read16(0x128)&4)!=internal);
        port.write16(0x128,bits);
        port.write16(0x202,0x80);
        return result;
    }
    void identify(unsigned i) {
        // Published logic-analyzer word sequence, with known responses.
        const std::uint32_t tx[]={0x7fff494e,0xffff494e,0xb6b1494e,0xb6b1544e,0xabb1544e,
            0xabb14e45,0xb1ba4e45,0xb1ba4f44,0xb0bb4f44,0xb0bb8001};
        const std::uint32_t rx[]={0,0x494eb6b1,0x494eb6b1,0x544eb6b1,0x544eabb1,
            0x4e45abb1,0x4e45b1ba,0x4f44b1ba,0x4f44b0bb,0x8001b0bb};
        for (unsigned n=0;n<10;++n) CHECK(shift(i,tx[n],false)==rx[n]);
    }
    std::vector<std::uint32_t> command(unsigned i,unsigned cmd,std::initializer_list<std::uint32_t> data={}) {
        CHECK(shift(i,0x99660000|(unsigned(data.size())<<8)|cmd)==0x80000000);
        for (auto word:data) CHECK(shift(i,word)==0x80000000);
        auto header=shift(i,0x80000000);
        CHECK((header&0xffff00ff)==(0x99660080|cmd));
        std::vector<std::uint32_t> result;
        for (unsigned n=0;n<((header>>8)&255);++n) result.push_back(shift(i,0x80000000));
        return result;
    }
};
void discover_connect_data() {
    Radio r;
    for (unsigned i=0;i<2;++i) { r.identify(i); r.command(i,0x10); r.command(i,0x17,{0x003c0420}); }
    r.command(0,0x16,{1,2,3,4,5,6}); r.command(0,0x19);
    r.command(1,0x1c);
    CHECK(r.command(1,0x1d)==std::vector<std::uint32_t>({0x6000,1,2,3,4,5,6}));
    r.command(1,0x1e); r.command(1,0x1f,{0x6000});
    CHECK(r.command(1,0x20)==std::vector<std::uint32_t>({0x6001}));
    CHECK(r.command(0,0x1a)==std::vector<std::uint32_t>({0x6001}));
    CHECK(r.command(1,0x11)==std::vector<std::uint32_t>({255}));
    // A child waits for the parent's radio transaction; the WAN is absent.
    r.command(1,0x25,{4u<<8,0x12345678});
    r.command(0,0x25,{4,0xabcdef01});
    const auto mid=r.domain.save_state();
    const auto finish=[&] {
        r.tick(4096);
        for (unsigned i=0;i<2;++i) {
            CHECK(r.shift(i,0x80000000,true,false)==0x99660028);
            CHECK(r.shift(i,0x996600a8,true,false)==0x80000000);
        }
        CHECK(r.command(0,0x26)==std::vector<std::uint32_t>({4u<<8,0x12345678}));
        CHECK(r.command(1,0x26)==std::vector<std::uint32_t>({4,0xabcdef01}));
        CHECK(r.command(1,0x26).empty());
        return r.domain.save_state();
    };
    const auto expected=finish();
    std::string error;
    CHECK(r.domain.load_state(mid,&error));
    CHECK(r.domain.save_state()==mid);
    CHECK(finish()==expected);
    CHECK(r.domain.load_state(mid,&error));
    auto broken=mid; broken.push_back(0);
    CHECK(!r.domain.load_state(broken,&error));
    CHECK(r.domain.save_state()==mid);
}
void multiple_groups() {
    Radio r(10); // Domain population is not the four-child or eight-leader limit.
    for (unsigned i=0;i<10;++i) r.identify(i);
    for (unsigned host:{0u,5u}) r.command(host,0x19);
    for (unsigned i=1;i<5;++i) {
        r.command(i,0x1f,{0x6000});
        r.command(i+5,0x1f,{0x6005});
    }
    CHECK(r.command(0,0x1a).size()==4); CHECK(r.command(5,0x1a).size()==4);
    r.command(0,0x30,{2});
    CHECK(r.command(0,0x1a).size()==3); CHECK(r.command(5,0x1a).size()==4);
    std::string error; auto state=r.domain.save_state(); CHECK(r.domain.load_state(state,&error));
}
void reset_timeout_and_detach() {
    Radio r;
    r.identify(0); r.identify(1);
    r.command(0,0x17,{1}); // one emulated frame, no host-clock timer
    r.command(0,0x27);
    auto before=r.domain.save_state();
    const auto timeout=[&] {
        r.tick(280896);
        CHECK(r.shift(0,0x80000000,true,false)==0x99660027);
        CHECK(r.shift(0,0x996600a7,true,false)==0x80000000);
        return r.domain.save_state();
    };
    const auto after=timeout();
    std::string error;
    CHECK(r.domain.load_state(before,&error)); CHECK(timeout()==after);
    r.command(0,0x3d);
    CHECK(r.shift(0,0x99660019)==0x80000000);
    CHECK(r.shift(0,0x80000000)==0x80000000); // sleep cannot start hosting
    r.io[0]->write16(0x134,0x8022);
    r.io[0]->write16(0x134,0);
    r.identify(0); r.command(0,0x19);
    r.command(1,0x1f,{0x6000});
    r.io[0]->set_serial_device(nullptr);
    CHECK(r.command(1,0x11)==std::vector<std::uint32_t>({0}));
    // A disconnected endpoint cannot hold back the remaining serial clock.
    r.command(1,0x19);
    CHECK(r.command(1,0x1a).empty());
}
}
int main() {
    discover_connect_data(); multiple_groups(); reset_timeout_and_detach();
    std::puts("RFU protocol, discovery, association, data, reset and snapshots passed");
}
