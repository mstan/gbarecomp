#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_native_region.h"
#include "runtime_bus_bridge.h"
#include "snapshot.h"
#include <memory>
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <cstring>

#define NATIVE_REGION_FULL_FUNCTION_TESTS 1
#include "n02_generated.inc"

struct ObservedWrites final:gba::BusWriteObserver {
    unsigned calls=0;
    bool on_bus_write(gba::BusWriteRegion,uint32_t,uint32_t,uint8_t,uint32_t,uint32_t) override {
        ++calls;return false;
    }
};
std::vector<uint8_t> run(unsigned mode,unsigned scenario,bool native) {
    auto bus=std::make_unique<gba::GbaBus>();auto ppu=std::make_unique<gba::GbaPpu>();
    std::vector<uint8_t> rom(0x20000);
    for(unsigned i=0;i<rom.size();++i)rom[i]=uint8_t(i*17+0x69);
    bus->set_rom(rom.data(),rom.size());bus->rtc().set_emulated_clock(946684800);
    bus->io().set_bus(bus.get());bus->io().set_ppu(ppu.get());
    bus->write32(0x02000100,0x81fedcba);bus->write32(0x0300100c,0x73abcdef);
    gbarecomp::set_active_bus(bus.get());gbarecomp::set_active_ppu(ppu.get());runtime_init(bus.get());
    gbarecomp::runtime_restore_timing_values({});runtime_call_stack_restore(nullptr,0);
    g_cpu={};g_cpu.cpsr=mode==0?0x2000003f:0x2000001f;
    g_cpu.R[0]=0x12873456;g_cpu.R[1]=0x76543210;g_cpu.R[2]=0x03001000;
    g_cpu.R[3]=3;g_cpu.R[4]=0x02000100;g_cpu.R[14]=0x08002000|(mode==0?1u:0u);
    g_cpu.R[15]=0x08001000;g_cpu.R[13]=0x03007000;
    g_cpu.banked_sp[2]=0xabcdef01;
    if(scenario==1)g_cpu.R[4]=0x08000100;
    if(scenario==2)g_cpu.R[4]=0x04000004;
    if(scenario==3)g_cpu.R[2]=0x04000100;
    if(scenario==4)g_cpu.R[2]=0x08010000;
    if(scenario==5)g_cpu.R[4]=0x0d000100;
    if(scenario==6)g_cpu.R[4]=0x080000c4;
    ObservedWrites observer;
    if(scenario==7)bus->set_write_observer(&observer);
    if(scenario==8) {
        g_runtime_resume_pc=0x08001000+4*(mode==0?2u:4u);
        g_cpu.R[15]=g_runtime_resume_pc;
    } else g_runtime_resume_pc=0;
    runtime_tick(1);
    runtime_call_push_return(0x08002000);
    const auto count_before=runtime_native_region_instruction_count();
    if(mode==0) {if(native)run_function_0_1();else run_function_0_0();}
    else {if(native)run_function_1_1();else run_function_1_0();}
    const auto executed=runtime_native_region_instruction_count()-count_before;
    if((!native || scenario==7) && executed!=0)
        throw std::runtime_error("maintained/observer path unexpectedly executed a native region");
    if(native && (scenario==0 || scenario==1 || scenario==8) && executed==0)
        throw std::runtime_error("native function test silently used only the maintained body");
    if(g_cpu.R[15]!=0x08002000 || runtime_call_stack_depth()!=0)
        throw std::runtime_error("generated function continuation was not restored");
    gbarecomp::debug::SnapshotWriter state;
    state.bytes(&g_cpu,sizeof(g_cpu));
    bus->serialize(state);bus->io().serialize(state);bus->audio().serialize(state);
    bus->save().serialize(state);ppu->serialize(state);
    gbarecomp::RuntimeTimingContext time;gbarecomp::runtime_capture_timing_context(time);
    state.u64(time.cycles);state.u64(time.pending_cycles);state.u64(uint64_t(time.event_budget));
    state.u64(time.vblank_starts);state.u64(time.yielded_vblank);state.u32(observer.calls);
    bus->set_write_observer(nullptr);
    gbarecomp::set_active_bus(nullptr);gbarecomp::set_active_ppu(nullptr);runtime_shutdown();
    return state.take_buffer();
}
int main() {
#if defined(GBA_COSIM)
    return 77;
#else
    try {
        for(unsigned mode=0;mode!=2;++mode) for(unsigned scenario=0;scenario!=9;++scenario) {
            const auto reference=run(mode,scenario,false),native=run(mode,scenario,true);
            if(reference!=native) {
                std::size_t first=0;
                while(first<reference.size() && first<native.size() && reference[first]==native[first])++first;
                std::fprintf(stderr,"mode=%u scenario=%u first_difference=%zu sizes=%zu/%zu\n",mode,scenario,first,reference.size(),native.size());
                throw std::runtime_error("full generated function machine state differs");
            }
        }
        std::puts("native full-function contract: 18 ARM/Thumb cases, complete serialized CPU/RAM/devices/timing, observer fallback and resume aliases exact");
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"native function FAILED: %s\n",error.what());return 1;}
#endif
}
