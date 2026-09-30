#pragma once
#include "multiplayer_session.h"
#include <cstdlib>

namespace link_fixture {
// Native ABI fixture for transport tests. Each frame's keys travel through the
// actual local cable; a rolling guest RAM digest makes stale input observable.
inline int program(std::uint32_t pc,int) {
    if (pc < 0x08000000 || pc > 0x08000018) return 0;
    if (runtime_should_yield()) return 1;
    g_cpu.R[15]=pc+4;
    switch ((pc-0x08000000)/4) {
    case 0: bus_write_u16(0x04000134,0); break;
    case 1: bus_write_u16(0x0400012a,g_cpu.R[0]+(~bus_read_u16(0x04000130)&0x3ff)); break;
    case 2: bus_write_u16(0x04000128,0x2003); break;
    case 3: if (g_cpu.R[0]==0x1111) bus_write_u16(0x04000128,0x2083); break;
    case 4: if (bus_read_u16(0x04000128)&0x80) g_cpu.R[15]=pc; break;
    case 5: {
        const auto words=bus_read_u32(0x04000120), high=bus_read_u32(0x04000124);
        bus_write_u32(0x02000000,words);
        bus_write_u32(0x02000004,high); // ports 2/3 on a larger cable, else ffffffff
        bus_write_u32(0x02000008,(((bus_read_u32(0x02000008)^words)*16777619u)^high)*16777619u);
        bus_write_u8(0x0e000000,~bus_read_u16(0x04000130)&0xff);
        break;
    }
    case 6:
        if (g_cpu.R[2]) {
            if (!g_cpu.R[3]) {
                bus_write_u16(0x04000200,8); // timer 0 wakes HALT; CPU IRQ stays masked
                bus_write_u16(0x04000100,65536-4389); // one frame at /64
                bus_write_u16(0x04000102,0xc1);
                g_cpu.R[3]=1;
            }
            bus_write_u16(0x04000202,0x88);
            g_cpu.R[15]=0x08000000;
        } else g_cpu.R[15]=pc;
        bus_write_u8(0x04000301,0);
        break;
    }
    runtime_tick(1);
    return 1;
}
// Two-process harness topology (tests/link/loopback.py): GBA_TEST_PLAYERS
// consoles; GBA_TEST_HUB makes seat 0 the LAN star every other seat dials.
inline int players() {
    const auto* value=std::getenv("GBA_TEST_PLAYERS");
    const int n=value ? std::atoi(value) : 2;
    return n>=2 && n<=4 ? n : 2;
}
inline bool hub(int slot) { return slot==0 && players()>2 && std::getenv("GBA_TEST_HUB"); }
// Each seat's owner wrote 0x90+seat to its own cartridge before startup.
inline bool owner_saves_intact(gbarecomp::GbaMultiplayerSession& s) {
    for (std::size_t seat=0;seat<s.input_count();++seat)
        if (s.input_machine(seat).bus.save().sram_read(1)!=0x90+seat) return false;
    return true;
}
// players: consoles on the cable (2..4). reverse_seats maps seat s to port N-1-s.
inline std::unique_ptr<gbarecomp::GbaMultiplayerSession> create(bool repeat=false,bool reverse_seats=false,
                                                                 unsigned players=2) {
    using namespace gbarecomp;
    GbaSessionConfig c;
    c.links={{GbaLinkMedium::Cable,{}}};
    for (unsigned port=0;port<players;++port) {
        c.machines.push_back({port,"net-test",std::string(40,'a')});
        c.links[0].machines.push_back(port);
        c.input_machines.push_back(reverse_seats ? players-1-port : port);
    }
    auto s=std::make_unique<GbaMultiplayerSession>(c);
    for (unsigned port=0;port<players;++port) {
        auto& m=s->machine(port);
        m.execution.program_dispatch=program; m.execution.cpu.R[15]=0x08000000;
        m.execution.cpu.R[0]=(port+1)*0x1111; m.execution.cpu.R[2]=repeat;
        m.bus.save().configure_sram();
    }
    return s;
}
}
