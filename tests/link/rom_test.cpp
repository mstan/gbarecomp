#include "multiplayer_session.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)
namespace {
gba::GbaLinkHub* callback_cable=nullptr;
unsigned callback_visits=0;
int ram_callback(std::uint32_t pc,int thumb) {
    CHECK(pc==0x03000000 && !thumb);
    CHECK(g_runtime_cycles==6);
    CHECK(callback_cable->cycle(0)==6 && callback_cable->cycle(1)==6);
    ++callback_visits;
    CHECK(!runtime_should_yield());
    ++g_cpu.R[0];
    runtime_tick(1);
    g_cpu.R[15]=g_cpu.R[14];
    return 1;
}
void native_ram_boundary(const std::vector<std::uint8_t>& rom) {
    using namespace gbarecomp;
    GbaSessionConfig config;
    config.machines={{0,"ram-boundary",std::string(40,'a')},{1,"ram-boundary",std::string(40,'a')}};
    config.input_machines={0,1}; config.links={{GbaLinkMedium::Cable,{0,1}}};
    auto session=std::make_unique<GbaMultiplayerSession>(config);
    for (auto id:{0,1}) {
        auto& m=session->machine(id);
        m.bus.set_rom(rom.data(),rom.size());
        m.execution.cpu.R[15]=0x08000128;
        m.execution.ram_dispatch=ram_callback;
    }
    const auto cold=session->save_state();
    callback_cable=&session->cable(); callback_visits=0;
    session->run_until(32); CHECK(callback_visits==2);
    const auto reference=session->save_state();
    std::string error; CHECK(session->load_state(cold,&error));
    callback_cable=&session->cable(); callback_visits=0;
    session->set_native_slices(true);
    session->run_until(6); CHECK(callback_visits==0);
    session->run_until(32); CHECK(callback_visits==2);
    CHECK(session->save_state()==reference);
    for (auto id:{0,1}) CHECK(session->machine(id).bus.read32(0x02000000)==42);
    callback_cable=nullptr;
}
}
int main(int argc, char** argv) {
    CHECK(argc == 2 || argc == 3);
    const bool trace = argc == 3 && std::string_view(argv[2])=="--trace";
    const bool hashes = argc == 3 && std::string_view(argv[2])=="--hashes";
    const bool normal_trace = argc == 3 && std::string_view(argv[2])=="--normal";
    CHECK(argc==2 || trace || hashes || normal_trace);
    std::ifstream file(argv[1], std::ios::binary);
    CHECK(file.good());
    const std::vector<std::uint8_t> rom{std::istreambuf_iterator<char>(file), {}};
    using namespace gbarecomp;
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        const bool normal=scenario>=4; const auto baud=scenario%4;
        if ((trace && normal) || (normal_trace && !normal)) continue;
        GbaSessionConfig config;
        config.machines = {{0,"link-fixture",std::string(40,'a')}, {1,"link-fixture",std::string(40,'a')}};
        config.input_machines = {0,1};
        config.links = {{GbaLinkMedium::Cable,{0,1}}};
        auto session = std::make_unique<GbaMultiplayerSession>(config);
        for (unsigned port = 0; port < 2; ++port) {
            auto& m = session->machine(port);
            m.bus.set_rom(rom.data(),rom.size());
            m.execution.cpu.R[15] = 0x08000000;
            m.execution.cpu.R[0] = (port+1)*0x1111;
            m.execution.cpu.R[1] = baud;
            m.execution.cpu.R[8] = 4096;
            m.execution.cpu.R[9] = normal;
        }
        const auto cold = session->save_state();
        while (!session->cable().transfer_active() && session->cycle() < 40000)
            session->run_until(session->cycle()+(normal ? 1 : 64));
        CHECK(session->cable().transfer_active());
        const auto before = session->save_state();
        session->run_until(100000);
        const auto after = session->save_state();
        for (unsigned port = 0; port < 2; ++port) {
            auto& bus = session->machine(port).bus;
            if (normal) {
                CHECK(bus.read32(0x02000000)==(port ? ((baud&2) ? 0x1111u : 0x11u) : ((baud&2) ? 0xffffffffu : 0xffu)));
                CHECK(bus.read32(0x02000004)==0);
                CHECK(bus.read32(0x02000008)==(0x4000u|((baud&2) ? 0x1000u : 0)|((baud&1)<<1)|(port ? 0 : 1)));
            } else {
                CHECK(bus.read32(0x02000000) == 0x22221111);
                CHECK(bus.read32(0x02000004) == 0xffffffff);
                CHECK(bus.read32(0x02000008) == (0x6008u | baud | (port ? 0x14 : 0)));
            }
            CHECK(bus.read32(0x0200000c) & 0x80);
            CHECK(bus.read32(0x02000010) == 0x42);
            if (trace || normal_trace) std::printf("%u,%u,%08x,%08x,%04x,%04x\n",baud,port,
                bus.read32(0x02000000),bus.read32(0x02000004),
                bus.read32(0x02000008),bus.read32(0x0200000c)&0x80);
        }
        std::string error;
        CHECK(session->load_state(before,&error));
        session->set_native_slices(true);
        session->run_until(100000);
        CHECK(session->save_state() == after);
        CHECK(session->load_state(cold,&error));
        session->run_until(100000);
        CHECK(session->save_state() == after);
        if (hashes) std::printf("%u,%08x\n",scenario,session->state_hash());
    }
    if (!trace && !hashes && !normal_trace) {
        native_ram_boundary(rom);
        std::puts("generated ROM multiplayer/normal exchanges, RAM boundary and replay passed");
    }
}
