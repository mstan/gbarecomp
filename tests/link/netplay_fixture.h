#pragma once
#include "multiplayer_session.h"

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
        const auto words=bus_read_u32(0x04000120);
        bus_write_u32(0x02000000,words);
        bus_write_u32(0x02000008,(bus_read_u32(0x02000008)^words)*16777619u);
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
inline std::unique_ptr<gbarecomp::GbaMultiplayerSession> create(bool repeat=false,bool reverse_seats=false) {
    using namespace gbarecomp;
    GbaSessionConfig c;
    c.machines={{0,"net-test",std::string(40,'a')},{1,"net-test",std::string(40,'a')}};
    c.links={{GbaLinkMedium::Cable,{0,1}}}; c.input_machines={0,1};
    if (reverse_seats) c.input_machines={1,0};
    auto s=std::make_unique<GbaMultiplayerSession>(c);
    for (unsigned port=0;port<2;++port) {
        auto& m=s->machine(port);
        m.execution.program_dispatch=program; m.execution.cpu.R[15]=0x08000000;
        m.execution.cpu.R[0]=(port+1)*0x1111; m.execution.cpu.R[2]=repeat;
        m.bus.save().configure_sram();
    }
    return s;
}
}
