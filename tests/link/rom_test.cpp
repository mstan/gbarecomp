#include "multiplayer_session.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)
namespace {
constexpr std::uint64_t kRamProbeEntry = 26;
// Past the return to ROM and the EWRAM store of the result (27 + mov 6 +
// str 14 = 47 on both consoles).
constexpr std::uint64_t kRamProbeDone = 64;
gba::GbaLinkHub* callback_cable=nullptr;
unsigned callback_visits=0;
int ram_callback(std::uint32_t pc,int thumb) {
    CHECK(pc==0x03000000 && !thumb);
    // Three ARM ROM instructions (1 + S32 fetch wait 5 at power-on WAITCNT)
    // and the BX into IWRAM (2S+1N with the ROM fetch wait, zero-wait IWRAM
    // refill): 3*6 + 8.
    CHECK(g_runtime_cycles==kRamProbeEntry);
    CHECK(callback_cable->cycle(0)==kRamProbeEntry && callback_cable->cycle(1)==kRamProbeEntry);
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
    session->run_until(kRamProbeDone); CHECK(callback_visits==2);
    const auto reference=session->save_state();
    std::string error; CHECK(session->load_state(cold,&error));
    callback_cable=&session->cable(); callback_visits=0;
    session->set_native_slices(true);
    session->run_until(kRamProbeEntry); CHECK(callback_visits==0);
    session->run_until(kRamProbeDone); CHECK(callback_visits==2);
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
    // --start: print the core cycle of the master's multiplayer start write
    // (S,baud,cycle), the native side of mGBA's `--timing` start time.
    const bool start_trace = argc == 3 && std::string_view(argv[2])=="--start";
    CHECK(argc==2 || trace || hashes || normal_trace || start_trace);
    std::ifstream file(argv[1], std::ios::binary);
    CHECK(file.good());
    const std::vector<std::uint8_t> rom{std::istreambuf_iterator<char>(file), {}};
    using namespace gbarecomp;
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        const bool normal=scenario>=4; const auto baud=scenario%4;
        if (((trace || start_trace) && normal) || (normal_trace && !normal)) continue;
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
        // The fixture's 4096-iteration ARM settle loop runs from ROM at the
        // power-on WAITCNT: 38 cycles per iteration (mGBA agrees exactly,
        // 155648 cycles), so the start lands after ~156k cycles.
        const std::uint64_t stride = (normal || start_trace) ? 1 : 64;
        while (!session->cable().transfer_active() && session->cycle() < 400000)
            session->run_until(session->cycle()+stride);
        CHECK(session->cable().transfer_active());
        if (start_trace)
            std::printf("S,%u,%llu\n",baud,
                static_cast<unsigned long long>(session->cable().cycle(0)));
        // Longest 2-console transfer is 63427 cycles (baud 0).
        const std::uint64_t horizon = session->cycle() + 70000;
        const auto before = session->save_state();
        session->run_until(horizon);
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
        session->run_until(horizon);
        CHECK(session->save_state() == after);
        CHECK(session->load_state(cold,&error));
        session->run_until(horizon);
        CHECK(session->save_state() == after);
        if (hashes) std::printf("%u,%08x\n",scenario,session->state_hash());
    }
    if (!trace && !hashes && !normal_trace && !start_trace) {
        native_ram_boundary(rom);
        std::puts("generated ROM multiplayer/normal exchanges, RAM boundary and replay passed");
    }
}
