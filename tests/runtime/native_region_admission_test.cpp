#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_native_region.h"
#include "runtime_bus_bridge.h"
#include <cstdio>
#include <memory>
#include <vector>
#include <cstring>
#include <stdexcept>

struct Observer final:gba::BusWriteObserver {
    bool on_bus_write(gba::BusWriteRegion,uint32_t,uint32_t,uint8_t,uint32_t,uint32_t) override {return false;}
};
int read_override(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t*) {return 0;}
int interp_override(uint32_t,int) {return 0;}
void require(bool condition,const char* what) {if(!condition)throw std::runtime_error(what);}

int main() {
#if defined(GBA_COSIM)
    return 77;
#else
    try {
        auto bus=std::make_unique<gba::GbaBus>();
        auto ppu=std::make_unique<gba::GbaPpu>();
        std::vector<uint8_t> rom(0x20000);
        rom[0x1006]=0x78;rom[0x1007]=0x56;
        bus->set_rom(rom.data(),rom.size());
        bus->io().set_bus(bus.get());bus->io().set_ppu(ppu.get());
        gbarecomp::set_active_bus(bus.get());gbarecomp::set_active_ppu(ppu.get());
        runtime_init(bus.get());
        g_cpu={};g_cpu.cpsr=0x3f;g_cpu.R[15]=0x08001000;
        g_cpu.banked_sp[2]=0xaabbccdd;
        runtime_tick(1); // establish the actual first device deadline
        gbarecomp::RuntimeTimingContext initial;
        gbarecomp::runtime_capture_timing_context(initial);
        RuntimeNativeRegion region{};
        require(runtime_native_region_begin(&region,2,0x08001000,1),"normal region was not admitted");
        const auto bound=region.bound;
        region.bus_write_u32(0x03000100,0x12345678);
        region.cpu.R[15]=0x08001002;region.runtime_tick(3);
        region.cpu.R[1]=region.bus_read_u32(0x03000100);
        region.cpu.arm_set_nz(0);
        region.cpu.R[15]=0x08001004;region.runtime_tick(3);
        region.commit();
        require(g_cpu.R[1]==0x12345678 && bus->read32(0x03000100)==0x12345678,"RAM/register settlement differs");
        require(g_cpu.R[15]==0x08001004 && (g_cpu.cpsr&CPSR_Z_BIT) && g_cpu.banked_sp[2]==0xaabbccdd,"CPU/banked settlement differs");
        require(bus->read32(0x01000000)==0x56785678,"last-read open-bus PC was not preserved");
        gbarecomp::RuntimeTimingContext after;
        gbarecomp::runtime_capture_timing_context(after);
        require(after.cycles==initial.cycles+6 && after.pending_cycles==initial.pending_cycles+6 && after.event_budget==initial.event_budget-6,"exact timing settlement differs");
        require(!region.owner_bus,"completed region retained ownership");
        g_cpu.R[15]=0x08001000;
        auto reject=[&](const char* why,uint32_t count=2,uint32_t pc=0x08001000,uint32_t thumb=1) {
            RuntimeNativeRegion sentinel{};sentinel.cpu.R[0]=0xabcdef01;sentinel.cycles=19;
            std::vector<unsigned char> bytes(sizeof(sentinel));std::memcpy(bytes.data(),&sentinel,sizeof(sentinel));
            require(!runtime_native_region_begin(&sentinel,count,pc,thumb),why);
            require(std::memcmp(bytes.data(),&sentinel,sizeof(sentinel))==0,"rejected admission changed native state");
        };
        Observer observer;bus->set_write_observer(&observer);reject("write observer was bypassed");bus->set_write_observer(nullptr);
        g_runtime_bus_read_override=read_override;reject("read override was bypassed");g_runtime_bus_read_override=nullptr;
        g_runtime_force_interp_hook=interp_override;reject("interpreter override was bypassed");g_runtime_force_interp_hook=nullptr;
        g_runtime_insn_trace=1;reject("instruction observer was bypassed");g_runtime_insn_trace=0;
        g_runtime_trace_enabled=1;reject("trace observer was bypassed");g_runtime_trace_enabled=0;
        gbarecomp::runtime_session_execution(true);reject("session owner was bypassed");gbarecomp::runtime_session_execution(false);
        reject("empty region was admitted",0);
        reject("foreign PC was admitted",2,0x08001002);
        reject("wrong instruction mode was admitted",2,0x08001000,0);
        auto constrained=after;constrained.event_budget=bound;
        gbarecomp::runtime_restore_timing_values(constrained);
        reject("equal deadline was crossed");
        constrained.event_budget=bound-1;gbarecomp::runtime_restore_timing_values(constrained);
        reject("short deadline was crossed");
        gbarecomp::runtime_restore_timing_values(after);
        bus->io().write16(0x200,1);bus->io().write16(0x208,1);bus->io().request_irq(1);
        reject("pending eligible IRQ was bypassed");
        bus->io().write16(0x202,1);bus->io().write16(0x200,0);bus->io().write16(0x208,0);
        require(runtime_native_region_begin(&region,2,0x08001000,1),"region failed after ordinary fallback conditions cleared");
        region.commit(); // zero-length completed prefix, e.g. first load is MMIO
        gbarecomp::set_active_bus(nullptr);gbarecomp::set_active_ppu(nullptr);runtime_shutdown();
        std::puts("native region admission: deadline, observers, session, IRQ, exact timing, RAM, banked state and last-read open-bus passed");
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"native region admission FAILED: %s\n",error.what());return 1;}
#endif
}
