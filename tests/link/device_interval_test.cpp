#include "multiplayer_session.h"
#include "gba_serial_device.h"
#include "snapshot.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
using namespace gbarecomp;
namespace {
GbaSessionConfig config() {
    GbaSessionConfig c;
    c.machines={{0,"devices",std::string(40,'a')},{1,"devices",std::string(40,'a')}};
    c.input_machines={0,1};c.links={{GbaLinkMedium::Cable,{0,1}}};return c;
}
void configure(GbaMultiplayerSession& s) {
    for (unsigned seat=0;seat<2;++seat) {
        auto& m=s.machine(seat);auto& io=m.bus.io();
        m.execution.cpu.R[4]=0x12340000+seat;
        m.execution.cpu.R[15]=0x08001000+seat*0x100;
        m.ppu.set_rasterization_enabled(false);
        for(unsigned i=0;i<0x2000;i+=4)m.bus.write32(0x02000000+i,0x817f0bf1u+i*0x01010101u+seat);
        io.write16(0x084,0x80);io.write16(0x080,0xff77);io.write16(0x082,0x730e);
        io.write16(0x062,0xf280);io.write16(0x064,0x84c0);
        io.write32(0x0bc,0x02000000);io.write32(0x0c0,0x040000a0);
        io.write16(0x0c4,4);io.write16(0x0c6,0xb640);
        io.write32(0x0c8,0x02001000);io.write32(0x0cc,0x040000a4);
        io.write16(0x0d0,4);io.write16(0x0d2,0xb640);
        io.write16(0x100,65536-257);io.write16(0x102,0x00c0);
        io.write16(0x104,65536-509);io.write16(0x106,0x0080);
        io.write8(0x301,0); // Halted CPUs isolate device ownership; IRQ flags still accrue.
    }
}
std::vector<std::uint8_t> audio(const GbaMultiplayerSession& s,
                              const std::array<std::uint64_t,2>& start,bool& audible) {
    debug::SnapshotWriter out;
    for(unsigned seat=0;seat<2;++seat) {
        const auto& a=s.machine(seat).bus.audio();
        const auto count=a.samples_generated()-start[seat];CHECK(count<4096);
        std::array<gba::GbaAudio::CapSample,4096> cap{};std::uint64_t first=0;
        CHECK(a.query_capture(start[seat],count,cap.data(),first)==count && first==start[seat]);
        out.u64(count);
        for(unsigned i=0;i<count;++i) {
            out.u16(cap[i].mixed);for(auto ch:cap[i].ch)out.u16(ch);
            out.u16(cap[i].direct_a);out.u16(cap[i].direct_b);
            audible|=cap[i].mixed!=0;
        }
    }
    return out.buffer();
}
std::vector<std::uint8_t> mmio(std::uint64_t start) {
    const auto count=gba::gba_mmio_cap_total()-start;CHECK(count<4096);
    std::array<gba::MmioCapEntry,4096> entries{};std::uint64_t first=0;
    CHECK(gba::gba_mmio_cap_query(start,count,entries.data(),first)==count && first==start);
    debug::SnapshotWriter out;
    for(unsigned i=0;i<count;++i) {
        const auto& e=entries[i];out.u64(e.cycle);out.u32(e.addr);
        out.u32(e.value);out.u32(e.size);out.u32(e.pc);
        CHECK(e.pc==0x08001000 || e.pc==0x08001100);
    }
    return out.buffer();
}
void differential(std::ofstream& trace) {
    auto reference=std::make_unique<GbaMultiplayerSession>(config());
    auto candidate=std::make_unique<GbaMultiplayerSession>(config());
    reference->set_owned_device_intervals(false);configure(*reference);configure(*candidate);
    bool audible=false;unsigned observations=0;
    for(auto target:std::array<std::uint64_t,8>{1,63,1008,6173,32000,64000,128000,300000}) {
        std::array<std::uint64_t,2> heads{};
        for(unsigned i=0;i<2;++i)heads[i]=reference->machine(i).bus.audio().samples_generated();
        auto first=gba::gba_mmio_cap_total();reference->run_until(target);const auto expected_mmio=mmio(first);
        first=gba::gba_mmio_cap_total();candidate->run_until(target);CHECK(mmio(first)==expected_mmio);
        const auto state=reference->save_state();CHECK(candidate->save_state()==state);
        const auto pcm=audio(*reference,heads,audible);CHECK(audio(*candidate,heads,audible)==pcm);
        auto size=static_cast<std::uint32_t>(state.size());trace.write(reinterpret_cast<char*>(&size),4);
        trace.write(reinterpret_cast<const char*>(state.data()),state.size());
        size=static_cast<std::uint32_t>(pcm.size());trace.write(reinterpret_cast<char*>(&size),4);
        trace.write(reinterpret_cast<const char*>(pcm.data()),pcm.size());++observations;
    }
    CHECK(audible);
    const auto saved=candidate->save_state();candidate->run_until(301333);const auto after=candidate->save_state();
    std::string error;CHECK(candidate->load_state(saved,&error));candidate->run_until(301333);
    CHECK(candidate->save_state()==after);
    std::printf("device intervals: %u full paired-state/PCM/MMIO observations and current-build restore\n",observations);
}
void context(const GbaInstance& m) {
    CHECK(active_bus()==&m.bus && active_ppu()==&m.ppu);
    CHECK(std::memcmp(&g_cpu,&m.execution.cpu,sizeof(g_cpu))==0);
    CHECK(g_runtime_cycles==m.timing.cycles);
}
struct Serial final:gba::GbaSerialDevice {
    GbaInstance& m;unsigned visits=0;bool fail=false;
    explicit Serial(GbaInstance& value):m(value){}
    std::uint16_t read16(std::uint32_t)const override{return 0;}
    void write16(std::uint32_t,std::uint16_t,std::uint16_t)override{}
    void tick(std::uint32_t)override{context(m);++visits;if(fail)throw std::runtime_error("serial sentinel");}
    std::uint32_t cycles_until_event()const override{return 17;}
};
struct Observer final:gba::GbaPpu::PresentationObserver {
    GbaInstance& m;unsigned visits=0;explicit Observer(GbaInstance& value):m(value){}
    void scanline(const gba::GbaPpu&,std::uint32_t,std::uint16_t,const std::uint8_t*,
                  const std::uint8_t*,const std::uint8_t*,const std::uint8_t*)override{context(m);++visits;}
    void frame_ready()override{context(m);++visits;}
    void restored()noexcept override{}
};
struct Writes final:gba::BusWriteObserver {
    GbaInstance& m;unsigned visits=0;explicit Writes(GbaInstance& value):m(value){}
    bool on_bus_write(gba::BusWriteRegion,std::uint32_t,std::uint32_t,std::uint8_t,
                      std::uint32_t,std::uint32_t)override{context(m);++visits;return false;}
};
struct DmaBus final:armv4t::Bus {
    GbaInstance& m;unsigned visits=0;explicit DmaBus(GbaInstance& value):m(value){}
    std::uint8_t read8(std::uint32_t a)override{context(m);++visits;return m.bus.read8(a);}
    std::uint16_t read16(std::uint32_t a)override{context(m);++visits;return m.bus.read16(a);}
    std::uint32_t read32(std::uint32_t a)override{context(m);++visits;return m.bus.read32(a);}
    void write8(std::uint32_t a,std::uint8_t v)override{context(m);++visits;m.bus.write8(a,v);}
    void write16(std::uint32_t a,std::uint16_t v)override{context(m);++visits;m.bus.write16(a,v);}
    void write32(std::uint32_t a,std::uint32_t v)override{context(m);++visits;m.bus.write32(a,v);}
};
void observations() {
    auto s=std::make_unique<GbaMultiplayerSession>(config());configure(*s);
    Observer a(s->machine(0));Writes b(s->machine(1));
    s->machine(0).ppu.set_presentation_observer(&a);s->machine(1).bus.set_write_observer(&b);
    s->run_until(2000);CHECK(a.visits && b.visits);
    s->machine(0).ppu.set_presentation_observer(nullptr);s->machine(1).bus.set_write_observer(nullptr);
    DmaBus dma(s->machine(0));s->machine(0).bus.io().set_bus(&dma);
    s->run_until(8000);CHECK(dma.visits);s->machine(0).bus.io().set_bus(&s->machine(0).bus);
    Serial serial(s->machine(0));s->machine(0).bus.io().set_serial_device(&serial);
    s->run_until(8100);CHECK(serial.visits);
    RuntimeArmContext before,after;RuntimeTimingContext t,u;
    runtime_capture_arm_context(before);runtime_capture_timing_context(t);auto* bus=active_bus();auto* ppu=active_ppu();
    serial.fail=true;bool threw=false;
    try{s->run_until(8200);}catch(const std::runtime_error& e){threw=std::string(e.what())=="serial sentinel";}
    CHECK(threw);runtime_capture_arm_context(after);runtime_capture_timing_context(u);
    CHECK(std::memcmp(&before.cpu,&after.cpu,sizeof(g_cpu))==0 && before.returns==after.returns);
    CHECK(t.cycles==u.cycles && t.vblank_starts==u.vblank_starts && t.yielded_vblank==u.yielded_vblank &&
          t.pending_cycles==u.pending_cycles && t.event_budget==u.event_budget);
    CHECK(active_bus()==bus && active_ppu()==ppu && !runtime_has_resumable_context());
    s->machine(0).bus.io().set_serial_device(nullptr);
}
void narrow_window() {
    auto s=std::make_unique<GbaMultiplayerSession>(config());configure(*s);auto& m=s->machine(0);
    RuntimeArmContext original;RuntimeTimingContext before;runtime_capture_arm_context(original);runtime_capture_timing_context(before);
    auto* bus=active_bus();auto* ppu=active_ppu();
    runtime_session_execution(true);
    CHECK(runtime_session_tick_owned_devices(m.bus,m.ppu,m.timing,m.execution.cpu.R[15],1));
    CHECK(std::memcmp(&g_cpu,&original.cpu,sizeof(g_cpu))==0);
    CHECK(!runtime_session_tick_owned_devices(m.bus,m.ppu,m.timing,m.execution.cpu.R[15],m.ppu.cycles_until_next_event()));
    runtime_session_execution(false);runtime_restore_arm_context(original);runtime_restore_timing_context(before);set_active_bus(bus);set_active_ppu(ppu);
}
}
int main(int argc,char** argv) {
    try {
#ifdef _WIN32
        _putenv_s("GBARECOMP_MMIO_CAP","1");
#else
        setenv("GBARECOMP_MMIO_CAP","1",1);
#endif
        std::ofstream trace(argc>1?argv[1]:"multiplayer-device-interval-trace.bin",std::ios::binary);CHECK(bool(trace));
        differential(trace);observations();narrow_window();CHECK(bool(trace));
        std::puts("multiplayer device intervals passed: exact PCM/state/MMIO, renderer/write/serial/DMA observers and exception caller restore");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"device intervals FAIL: %s\n",e.what());return 1;}
}
