// runtime_bus_bridge.cpp — overrides the weak bus-accessor stubs in
// runtime_arm.cpp with real implementations that delegate to the
// active gba::GbaBus.
//
// The runner's main() must call `gbarecomp::set_active_bus(&bus)`
// before any recompiled cart code executes.

#include "../armv4t/runtime_arm.h"
#include "../gba/gba_bus.h"

namespace gbarecomp {

static gba::GbaBus* g_active_bus = nullptr;

void set_active_bus(gba::GbaBus* bus) {
    g_active_bus = bus;
}

gba::GbaBus* active_bus() {
    return g_active_bus;
}

}  // namespace gbarecomp

extern "C" uint32_t bus_read_u32(uint32_t addr) {
    return gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read32(addr)
        : 0u;
}

extern "C" uint16_t bus_read_u16(uint32_t addr) {
    return gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read16(addr)
        : uint16_t{0};
}

extern "C" uint8_t bus_read_u8(uint32_t addr) {
    return gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read8(addr)
        : uint8_t{0};
}

extern "C" void bus_write_u32(uint32_t addr, uint32_t val) {
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write32(addr, val);
}

extern "C" void bus_write_u16(uint32_t addr, uint16_t val) {
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write16(addr, val);
}

extern "C" void bus_write_u8(uint32_t addr, uint8_t val) {
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write8(addr, val);
}
