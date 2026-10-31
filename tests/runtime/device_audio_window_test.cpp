#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "snapshot.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main(int argc,char** argv) {
    try {
        auto bus=std::make_unique<gba::GbaBus>();
        auto ppu=std::make_unique<gba::GbaPpu>();
        std::vector<uint8_t> rom(0x2000);
        bus->set_rom(rom.data(),rom.size());
        bus->rtc().set_emulated_clock(0);
        bus->io().set_bus(bus.get());bus->io().set_ppu(ppu.get());
        ppu->set_rasterization_enabled(false);
        gbarecomp::set_active_bus(bus.get());gbarecomp::set_active_ppu(ppu.get());
        runtime_init(bus.get());
        g_cpu={};g_cpu.cpsr=0xdf;g_cpu.R[15]=0x08000100;
        bus->audio().tick(0);
        gbarecomp::runtime_restore_timing_context({});
        runtime_resync_horizon();
        std::ofstream trace(argc>1?argv[1]:"device-audio-window-trace.bin",std::ios::binary);
        require(bool(trace),"cannot create scheduler trace");
        unsigned observations=0;bool audible=false;
        auto record=[&] {
            runtime_mmio_catch_up();
            gbarecomp::RuntimeTimingContext timing;
            gbarecomp::runtime_capture_timing_context(timing);
            require(timing.pending_cycles==0,"observation did not settle pending device time");
            gbarecomp::debug::SnapshotWriter writer;
            writer.u64(timing.cycles);writer.u64(timing.vblank_starts);
            writer.u64(timing.yielded_vblank);writer.u64(timing.pending_cycles);
            // The future scheduler horizon is implementation-owned. All CPU,
            // memory, device state and produced PCM below must match exactly.
            for (auto value:g_cpu.R) writer.u32(value);
            writer.u32(g_cpu.cpsr);
            std::array<int16_t,32768> pcm{};
            auto count=bus->audio().drain_samples(pcm.data(),pcm.size());
            writer.u32(static_cast<uint32_t>(count));
            writer.bytes(pcm.data(),count*sizeof(int16_t));
            audible|=std::any_of(pcm.begin(),pcm.begin()+count,[](int16_t value){return value!=0;});
            bus->serialize(writer);bus->io().serialize(writer);
            bus->audio().serialize(writer);ppu->serialize(writer);
            auto size=static_cast<uint32_t>(writer.size());
            trace.write(reinterpret_cast<const char*>(&size),sizeof(size));
            trace.write(reinterpret_cast<const char*>(writer.buffer().data()),writer.size());
            require(bool(trace),"scheduler trace write failed");++observations;
        };
        auto write16=[&](uint32_t offset,uint16_t value) {
            runtime_mmio_catch_up();bus->io().write16(offset,value);runtime_resync_horizon();
        };
        auto write32=[&](uint32_t offset,uint32_t value) {
            runtime_mmio_catch_up();bus->io().write32(offset,value);runtime_resync_horizon();
        };
        bus->audio().configure_shadow({gba::Mp2kSig{0,0x03002601}},
            rom.data(),rom.size(),bus->ewram_ptr(),0x40000,bus->iwram_ptr(),0x8000,false);
        runtime_tick(gba::GbaPpu::kDotsVisible*gba::GbaPpu::kCyclesPerDot);
        runtime_tick(4);
        gbarecomp::RuntimeTimingContext before,after;
        gbarecomp::runtime_capture_timing_context(before);
        std::printf("toggle setup cycles=%llu pending=%llu budget=%lld ppu=%u sample=%u shared=%u shadow=%d\n",
            static_cast<unsigned long long>(before.cycles),static_cast<unsigned long long>(before.pending_cycles),
            static_cast<long long>(before.event_budget),ppu->cycles_until_next_event(),
            bus->audio().cycles_until_next_sample(),bus->audio().cycles_until_next_shared_event(),bus->audio().shadow_enabled());
        require(before.pending_cycles!=0,"Shadow toggle did not exercise pending cycles");
        runtime_mmio_catch_up();bus->audio().set_shadow_enabled(true);runtime_resync_horizon();
        gbarecomp::runtime_capture_timing_context(after);
        require(after.pending_cycles==0 && after.cycles==before.cycles,
                "enabling Shadow changed the clock or left time unsettled");
        require(after.event_budget==12,"enabling Shadow did not re-arm the imminent sample barrier");
        record();runtime_tick(1024);runtime_tick(5);
        gbarecomp::runtime_capture_timing_context(before);
        require(before.pending_cycles!=0,"disabling Shadow did not exercise pending cycles");
        runtime_mmio_catch_up();bus->audio().set_shadow_enabled(false);runtime_resync_horizon();
        gbarecomp::runtime_capture_timing_context(after);
        require(after.pending_cycles==0 && after.cycles==before.cycles,
                "disabling Shadow changed the clock or left time unsettled");
#if GBARECOMP_TEST_DEVICE_AUDIO_WINDOWS
        require(after.event_budget>bus->audio().cycles_until_next_sample(),
                "disabling Shadow did not return internal samples to audio ownership");
#else
        require(after.event_budget==bus->audio().cycles_until_next_sample(),
                "reference sample deadline unexpectedly changed");
#endif
        record();
        for (unsigned i=0;i<0x2000;i+=4)
            bus->write32(0x02000000+i,0x817f0bf1u+i*0x01010101u);
        write16(0x084,0x0080);write16(0x080,0xff77);write16(0x082,0x730e);
        write16(0x062,0xf280);write16(0x064,0x84c0);
        write16(0x078,0xf200);write16(0x07c,0x802b);
        write32(0x0bc,0x02000000);write32(0x0c0,0x040000a0);
        write16(0x0c4,4);write16(0x0c6,0xb640);
        write32(0x0c8,0x02001000);write32(0x0cc,0x040000a4);
        write16(0x0d0,4);write16(0x0d2,0xb640);
        write16(0x100,static_cast<uint16_t>(65536-257));write16(0x102,0x00c0);
        write16(0x104,static_cast<uint16_t>(65536-509));write16(0x106,0x0080);
        constexpr std::array<uint32_t,8> spans{1,63,512,1006,1025,6173,1024,2077};
        for (unsigned event=0;event<32;++event) {
            runtime_tick(spans[event%spans.size()]);
            if (event%5==0) write16(0x088,0x0200|(((event/5)&3)<<14));
            if (event%7==0) write16(0x064,0x8400|event*11);
            if (event%11==0) write16(0x100,static_cast<uint16_t>(65536-263-event));
            if (event%4==3) record();
        }
        require(audible,"scheduler trace never produced audible PCM");
        std::printf("device audio windows: %u complete CPU/device/PCM observations, pending-cycle Shadow toggles, FIFO DMA, rates and timer IRQs\n",observations);
        gbarecomp::set_active_bus(nullptr);gbarecomp::set_active_ppu(nullptr);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"device audio windows FAIL: %s\n",error.what());return 1;
    }
}
