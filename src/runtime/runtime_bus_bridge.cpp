// runtime_bus_bridge.cpp — overrides the weak bus-accessor stubs in
// runtime_arm.cpp with real implementations that delegate to the
// active gba::GbaBus.
//
// The runner's main() must call `gbarecomp::set_active_bus(&bus)`
// before any recompiled cart code executes.

#include "../armv4t/runtime_arm.h"
#include "../gba/gba_bus.h"
#include "../gba/gba_ppu.h"

namespace gbarecomp {

static gba::GbaBus* g_active_bus = nullptr;
static gba::GbaPpu* g_active_ppu = nullptr;

bool should_trace_unmapped_read(uint32_t addr) {
    return (addr >> 24) >= 0x0Eu;
}

void trace_unmapped_read(uint32_t addr, uint32_t value, uint32_t width) {
    if (should_trace_unmapped_read(addr)) {
        runtime_trace_event(RUNTIME_TRACE_MEM_READ, g_cpu.R[15], addr, value,
                            width);
    }
}

void sync_bios_access() {
    if (g_active_bus) {
        g_active_bus->set_bios_access_enabled(g_cpu.R[15] < 0x00004000u);
    }
}

void set_active_bus(gba::GbaBus* bus) {
    g_active_bus = bus;
}

void set_active_ppu(gba::GbaPpu* ppu) {
    g_active_ppu = ppu;
}

gba::GbaBus* active_bus() {
    return g_active_bus;
}

}  // namespace gbarecomp

extern "C" uint32_t bus_read_u32(uint32_t addr) {
    gbarecomp::sync_bios_access();
    uint32_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read32(addr)
        : 0u;
    gbarecomp::trace_unmapped_read(addr, v, 4u);
    return v;
}

extern "C" uint16_t bus_read_u16(uint32_t addr) {
    gbarecomp::sync_bios_access();
    uint16_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read16(addr)
        : uint16_t{0};
    gbarecomp::trace_unmapped_read(addr, v, 2u);
    return v;
}

extern "C" uint8_t bus_read_u8(uint32_t addr) {
    gbarecomp::sync_bios_access();
    uint8_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read8(addr)
        : uint8_t{0};
    gbarecomp::trace_unmapped_read(addr, v, 1u);
    return v;
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

extern "C" void runtime_tick(uint32_t cycles) {
    auto* bus = gbarecomp::g_active_bus;
    auto* ppu = gbarecomp::g_active_ppu;
    if (!bus || !ppu || cycles == 0) return;

    uint32_t remaining = cycles;
    while (remaining != 0) {
        uint32_t chunk = remaining;
        uint32_t until_sample = bus->audio().cycles_until_next_sample();
        uint32_t until_timer = bus->io().cycles_until_next_timer_event();
        uint32_t until_ppu = ppu->cycles_until_next_event();
        if (until_sample < chunk) chunk = until_sample;
        if (until_timer < chunk) chunk = until_timer;
        if (until_ppu < chunk) chunk = until_ppu;
        if (chunk == 0) chunk = 1;

        bus->audio().tick(chunk);
        bus->io().tick_timers(chunk);

        uint16_t vc_compare = static_cast<uint16_t>(
            (bus->io().dispstat() >> 8) & 0xFFu);
        auto events = ppu->tick(chunk, vc_compare);
        uint16_t ds = bus->io().dispstat();
        if (events.hblank_started &&
            ppu->vcount() < gba::GbaPpu::kLinesVisible) {
            ppu->render_scanline(ppu->vcount(),
                                 bus->io().read16(0x000),
                                 bus->io().raw(),
                                 bus->vram_ptr(),
                                 bus->oam_ptr(),
                                 bus->pal_ptr());
        }
        if (events.vblank_started) {
            ppu->mark_framebuffer_latched();
        }
        if (events.vblank_started && (ds & 0x0008u)) {
            bus->io().request_irq(gba::GbaIo::IrqVBlank);
        }
        if (events.hblank_started && (ds & 0x0010u)) {
            bus->io().request_irq(gba::GbaIo::IrqHBlank);
        }
        if (events.vcount_matched && (ds & 0x0020u)) {
            bus->io().request_irq(gba::GbaIo::IrqVCount);
        }

        remaining -= chunk;
    }

    if (bus->io().irq_pending() && (g_cpu.cpsr & CPSR_I_BIT) == 0) {
        if (bus->io().halted()) bus->io().clear_halt();
        runtime_irq(g_cpu.R[15]);
    }
}

extern "C" bool runtime_should_yield(void) {
    auto* bus = gbarecomp::g_active_bus;
    return bus && bus->io().halted();
}
