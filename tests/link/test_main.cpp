#include "gba_io.h"
#include "gba_link_hub.h"
#include "multiplayer_config.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)

namespace {
struct Cable {
    std::array<gba::GbaIo, 4> io;
    gba::GbaLinkHub hub;
    explicit Cable(unsigned players) : hub(players) {
        for (unsigned i = 0; i < players; ++i) {
            io[i].set_serial_device(&hub.endpoint(i));
            io[i].write16(0x134, 0);
            io[i].write16(0x128, 0x6000);
            io[i].write16(0x12a, static_cast<std::uint16_t>(0x1000 + i));
        }
    }
    void tick(std::uint32_t cycles) {
        for (std::size_t i = 0; i < hub.port_count(); ++i) io[i].tick_sio(cycles);
    }
};
void transfer_matrix() {
    // Oracle observations fixed as expected values, independent of hub API.
    const unsigned durations[4][3] = {{63427,94884,125829},{16241,24104,31457},
                                     {10998,16241,20972},{5755,8376,10486}};
    for (unsigned players = 2; players <= 4; ++players) {
        for (unsigned baud = 0; baud < 4; ++baud) {
            Cable c(players);
            for (unsigned i = 0; i < players; ++i) {
                CHECK((c.io[i].read16(0x128) & 0x3c) == ((i << 4) | (i ? 4 : 0) | 8));
            }
            c.io[1].write16(0x128, 0x6080 | baud);
            CHECK(!c.hub.transfer_active());
            c.io[0].write16(0x128, 0x6080 | baud);
            CHECK(c.hub.transfer_active());
            const unsigned duration = durations[baud][players - 2];
            CHECK(c.hub.completion_cycle() == duration);
            c.io[1].write16(0x12a, 0xabcd); // only the NEXT transfer sees this
            c.tick(duration - 1);
            for (unsigned i = 0; i < players; ++i) {
                CHECK(c.io[i].read16(0x128) & 0x80);
                CHECK(!(c.io[i].read16(0x202) & 0x80));
                CHECK(c.io[i].read32(0x120) == 0xffffffff);
            }
            // One endpoint at completion must wait for every participant.
            c.io[0].tick_sio(1);
            CHECK(c.hub.transfer_active());
            CHECK(c.io[0].cycles_until_next_sio_event() == 0);
            for (unsigned i = 1; i < players; ++i) c.io[i].tick_sio(1);
            CHECK(!c.hub.transfer_active());
            for (unsigned i = 0; i < players; ++i) {
                CHECK(!(c.io[i].read16(0x128) & 0x80));
                CHECK(c.io[i].read16(0x202) & 0x80);
                for (unsigned slot = 0; slot < 4; ++slot)
                    CHECK(c.io[i].read16(0x120 + 2 * slot) == (slot < players ? 0x1000 + slot : 0xffff));
                c.io[i].write16(0x202, 0x80);
                c.io[i].write16(0x128, 0x2000 | baud); // disable IRQ
            }
            c.io[0].write8(0x128, 0x80 | baud);
            c.tick(duration);
            CHECK(c.io[0].read16(0x122) == 0xabcd);
            CHECK(!(c.io[0].read16(0x202) & 0x80));
            CHECK(!(c.io[1].read16(0x202) & 0x80));
        }
    }
}
void snapshots_and_barriers() {
    Cable c(2);
    c.io[0].tick_sio(3);
    bool caught = false;
    try { c.io[0].write16(0x128, 0x6083); } catch (const std::logic_error&) { caught = true; }
    CHECK(caught && !c.hub.transfer_active());
    c.io[1].tick_sio(3);
    c.io[0].write16(0x128, 0x6083);
    c.tick(100);
    const auto baseline = c.hub.save_state();
    auto malformed = baseline;
    malformed[0] ^= 1;
    std::string error;
    CHECK(!c.hub.load_state(malformed, &error));
    CHECK(c.hub.save_state() == baseline);
    for (std::size_t size = 0; size < baseline.size(); ++size)
        CHECK(!c.hub.load_state(std::span(baseline.data(), size), &error));
    c.tick(5655);
    const auto final = c.hub.save_state();
    CHECK(c.hub.load_state(baseline, &error));
    c.tick(5655);
    CHECK(c.hub.save_state() == final);
    c.io[0].write16(0x128, 0x6083);
    caught = false;
    try { c.io[0].tick_sio(5756); } catch (const std::logic_error&) { caught = true; }
    CHECK(caught);
    c.io[1].write16(0x134, 0x8000);
    CHECK(!c.hub.transfer_active());
    CHECK(!(c.io[0].read16(0x128) & 8));
    c.io[1].set_serial_device(nullptr);
    CHECK(!c.hub.load_state(baseline, &error));
    // Both destruction orders must remove live wiring.
    gba::GbaIo standalone;
    { gba::GbaLinkHub hub(2); standalone.set_serial_device(&hub.endpoint(0)); }
    CHECK(standalone.serial_device() == nullptr);
    gba::GbaLinkHub hub(2);
    { gba::GbaIo temporary; temporary.set_serial_device(&hub.endpoint(0)); }
    CHECK(!hub.endpoint(0).connected());
}
void legacy_serial() {
    gba::GbaIo io;
    io.write16(0x128, 0x4081);
    io.tick_sio(511);
    CHECK(io.read16(0x128) & 0x80);
    io.tick_sio(1);
    CHECK(!(io.read16(0x128) & 0x80));
    CHECK(io.read32(0x120) == 0xffffffff);
    CHECK(io.read16(0x202) & 0x80);
    Cable cable(2);
    for (auto& port:cable.io) {
        port.write16(0x134,0);
        port.write16(0x128,0x5088); // BIOS: 32-bit external clock, armed, IRQ enabled
    }
    cable.tick(100000);
    CHECK(!cable.hub.transfer_active());
    CHECK(cable.io[0].read16(0x128)&0x80);
    CHECK(!(cable.io[0].read16(0x202)&0x80));
    CHECK(!(cable.io[1].read16(0x202)&0x80));
    cable.io[0].write16(0x128,0x5089);
    cable.tick(2048);
    CHECK(!(cable.io[0].read16(0x128)&0x80));
    CHECK(cable.io[0].read16(0x202)&0x80);
}
void normal_serial() {
    for (unsigned players=2;players<=4;++players) for (unsigned wide=0;wide<2;++wide)
        for (unsigned fast=0;fast<2;++fast) {
            Cable c(players);
            const auto mode=0x4000|(wide ? 0x1000 : 0)|(fast ? 2 : 0);
            for (unsigned i=0;i<players;++i) {
                c.io[i].write16(0x128,mode);
                if (wide) c.io[i].write32(0x120,0x12345678+i);
                else c.io[i].write8(0x12a,0x70+i);
                if (i) c.io[i].write16(0x128,mode|0x80); // externally clocked receiver
            }
            c.io[0].write16(0x128,mode|0x81);
            const unsigned duration=(wide ? 32 : 8)*(fast ? 8 : 64);
            CHECK(c.hub.completion_cycle()==duration);
            c.tick(duration-1);
            CHECK(c.io[0].read16(0x128)&0x80);
            CHECK(!(c.io[0].read16(0x202)&0x80));
            const auto before=c.hub.save_state();
            c.tick(1);
            const auto after=c.hub.save_state();
            for (unsigned i=0;i<players;++i) {
                CHECK(!(c.io[i].read16(0x128)&0x80));
                CHECK(c.io[i].read16(0x202)&0x80);
                if (wide) CHECK(c.io[i].read32(0x120)==(i ? 0x12345678+i-1 : 0xffffffff));
                else CHECK(c.io[i].read8(0x12a)==(i ? 0x70+i-1 : 0xff));
            }
            std::string error;
            CHECK(c.hub.load_state(before,&error)); c.tick(1);
            CHECK(c.hub.save_state()==after);
        }
    Cable c(2);
    for (unsigned i=0;i<2;++i) c.io[i].write16(0x128,0x5001);
    c.io[1].write16(0x128,0x5081);
    CHECK(!c.hub.transfer_active()); // secondary arming does not clock this cable
    c.io[0].write16(0x128,0x5081);
    c.tick(2048);
    CHECK(c.io[0].read16(0x202)&0x80);
    CHECK(c.io[1].read16(0x202)&0x80);
}
void topology() {
    using namespace gbarecomp;
    GbaSessionConfig config;
    for (unsigned i = 0; i < 2; ++i) {
        config.machines.push_back({100 + i, "emerald", std::string(40, 'a')});
        config.input_machines.push_back(100 + i);
    }
    config.links.push_back({GbaLinkMedium::Cable, {100,101}});
    std::string error;
    CHECK(validate_cable_mvp(config, &error));
    config.machines[1].program_id = "firered";
    CHECK(validate_session_config(config, &error));
    CHECK(!validate_cable_mvp(config, &error));
    config.machines[1].boot = GbaBootSource::Multiboot;
    CHECK(validate_session_config(config, &error));
    CHECK(!validate_cable_mvp(config, &error));
    // Radio discovery domains must not inherit either cable port counts or
    // the shared network library's current eight-seat implementation limit.
    config.links[0].medium = GbaLinkMedium::Wireless;
    for (unsigned i = 2; i < 40; ++i) {
        config.machines.push_back({100 + i, "emerald", std::string(40, 'a')});
        config.links[0].machines.push_back(100 + i);
    }
    CHECK(validate_session_config(config, &error));
    CHECK(!validate_cable_mvp(config, &error));
    config.links[0].medium = GbaLinkMedium::Cable;
    CHECK(!validate_session_config(config, &error));
}
}
int main() {
    transfer_matrix(); snapshots_and_barriers(); legacy_serial(); normal_serial(); topology();
    std::puts("link tests passed");
}
