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

namespace {
// Cable transfer length in CPU cycles [baud][players-2]. These are the
// durations the pinned mGBA oracle schedules (`--timing`, docs/MULTIPLAYER.md).
constexpr std::uint32_t kSpan[4][3] = {
    {63427, 94884, 125829}, {16241, 24104, 31457}, {10998, 16241, 20972}, {5755, 8376, 10486}};

struct Options {
    bool trace = false, hashes = false, normal_trace = false, timing = false, start = false;
    unsigned players = 0; // 0: every supported count in the self-test
};

// Word each port offers. Normal serial: the fixture treats r0 bit 13 as the
// "external clock" role select, so every port after the clock owner needs it
// set (0x4444 alone would make port 3 a second clock master).
std::uint32_t outgoing(unsigned port, bool normal) {
    return (port + 1) * 0x1111u | ((normal && port) ? 0x2000u : 0u);
}

void run_players(const std::vector<std::uint8_t>& rom, unsigned players, const Options& opt) {
    using namespace gbarecomp;
    // The cycle-accurate ROM settle loop now lasts about 156k cycles. Measure
    // the transfer start, then run far enough to finish the longest cable span.
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        const bool normal = scenario >= 4; const auto baud = scenario % 4;
        if (((opt.trace || opt.timing || opt.start) && normal) || (opt.normal_trace && !normal)) continue;
        GbaSessionConfig config;
        std::vector<GbaMachineId> ids;
        for (unsigned port = 0; port < players; ++port) {
            config.machines.push_back({static_cast<GbaMachineId>(port), "link-fixture", std::string(40, 'a')});
            ids.push_back(static_cast<GbaMachineId>(port));
        }
        config.input_machines = ids;
        config.links = {{GbaLinkMedium::Cable, ids}};
        auto session = std::make_unique<GbaMultiplayerSession>(config);
        for (unsigned port = 0; port < players; ++port) {
            auto& m = session->machine(port);
            m.bus.set_rom(rom.data(), rom.size());
            m.execution.cpu.R[15] = 0x08000000;
            m.execution.cpu.R[0] = outgoing(port, normal);
            m.execution.cpu.R[1] = baud;
            m.execution.cpu.R[8] = 4096;
            m.execution.cpu.R[9] = normal;
        }
        const auto cold = session->save_state();
        // Timing mode resolves the start to one instruction; the self-test
        // uses 64-cycle strides for multiplayer, which bounds the same value.
        const std::uint64_t stride = (normal || opt.timing || opt.start) ? 1 : 64;
        while (!session->cable().transfer_active() && session->cycle() < 400000)
            session->run_until(session->cycle() + stride);
        CHECK(session->cable().transfer_active());
        if (opt.start)
            std::printf("S,%u,%llu\n", baud,
                static_cast<unsigned long long>(session->cable().cycle(0)));
        const auto span = session->cable().completion_cycle() - session->cable().cycle(0);
        const auto finish = session->cable().completion_cycle();
        const auto horizon = session->cycle() +
            (players == 2 ? 70000u : kSpan[baud][players - 2] + 4096u);
        if (!normal) {
            // Late detection only shortens the remaining span, by less than
            // the stride plus one instruction.
            CHECK(span <= kSpan[baud][players - 2]);
            CHECK(kSpan[baud][players - 2] - span < 2 * stride + 32);
        }
        const auto before = session->save_state();
        session->run_until(horizon);
        const auto after = session->save_state();
        for (unsigned port = 0; port < players; ++port) {
            auto& bus = session->machine(port).bus;
            if (normal) {
                const bool wide = baud & 2;
                const std::uint32_t received = port ? outgoing(port - 1, true) : 0xffffffffu;
                CHECK(bus.read32(0x02000000) == (wide ? received : (received & 0xffu)));
                CHECK(bus.read32(0x02000004) == 0);
                CHECK(bus.read32(0x02000008) == (0x4000u | (wide ? 0x1000u : 0) | ((baud & 1) << 1) | (port ? 0 : 1)));
            } else {
                // Every port receives all N words; unused slots read 0xffff.
                auto word = [&](unsigned slot) { return slot < players ? outgoing(slot, false) : 0xffffu; };
                CHECK(bus.read32(0x02000000) == (word(0) | (word(1) << 16)));
                CHECK(bus.read32(0x02000004) == (word(2) | (word(3) << 16)));
                // Ready, IRQ, multiplayer mode; slave bit and ID in bits 2/4-5.
                CHECK(bus.read32(0x02000008) == (0x6008u | baud | (port ? (0x4u | (port << 4)) : 0u)));
            }
            CHECK(bus.read32(0x0200000c) & 0x80);
            CHECK(bus.read32(0x02000010) == 0x42);
            if (opt.trace || opt.normal_trace || opt.timing)
                std::printf("%u,%u,%08x,%08x,%04x,%04x\n", baud, port,
                    bus.read32(0x02000000), bus.read32(0x02000004),
                    bus.read32(0x02000008), bus.read32(0x0200000c) & 0x80);
        }
        // T,baud,players,span,finish[,skew...]: hub-level completion timing
        // (see oracle/link/main.c for the mGBA-side definition of each field).
        if (opt.timing) {
            std::printf("T,%u,%u,%llu,%llu", baud, players,
                static_cast<unsigned long long>(span), static_cast<unsigned long long>(finish));
            for (unsigned port = 1; port < players; ++port) std::printf(",0");
            std::printf("\n");
        }
        std::string error;
        CHECK(session->load_state(before, &error));
        session->set_native_slices(true);
        session->run_until(horizon);
        CHECK(session->save_state() == after);
        CHECK(session->load_state(cold, &error));
        session->run_until(horizon);
        CHECK(session->save_state() == after);
        if (opt.hashes) std::printf("%u,%08x\n", scenario + 8 * (players - 2), session->state_hash());
    }
}
} // namespace

int main(int argc, char** argv) {
    CHECK(argc >= 2);
    Options opt;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--trace") opt.trace = true;
        else if (arg == "--hashes") opt.hashes = true;
        else if (arg == "--normal") opt.normal_trace = true;
        else if (arg == "--timing") opt.timing = true;
        else if (arg == "--start") opt.start = true;
        else if (arg == "--players") {
            CHECK(++i < argc);
            opt.players = static_cast<unsigned>(std::atoi(argv[i]));
            CHECK(opt.players >= 2 && opt.players <= 4);
        } else CHECK(false && "unknown argument");
    }
    std::ifstream file(argv[1], std::ios::binary);
    CHECK(file.good());
    const std::vector<std::uint8_t> rom{std::istreambuf_iterator<char>(file), {}};
    const bool self_test = !opt.trace && !opt.hashes && !opt.normal_trace && !opt.timing && !opt.start;
    // Trace/hash/timing modes default to the historical two-player cable.
    const unsigned first = opt.players ? opt.players : 2;
    const unsigned last = opt.players ? opt.players : (self_test ? 4 : 2);
    for (unsigned players = first; players <= last; ++players) run_players(rom, players, opt);
    if (self_test && !opt.players) {
        native_ram_boundary(rom);
        std::puts("generated ROM multiplayer/normal exchanges (2-4 players), RAM boundary and replay passed");
    }
}
