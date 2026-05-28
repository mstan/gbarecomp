// runtime_bus_bridge.h — public surface for binding the active bus
// to the recompiled-code runtime.

#pragma once

namespace gba { class GbaBus; }
namespace gba { class GbaPpu; }

// Count of PPU VBlank-start events (scanline 159->160), incremented in
// runtime_tick. The debug step-one-frame primitive stops on its increment
// so the recomp's TCP `step` parks at VBlank-start, matching the
// interpreter and mGBA oracles. Defined in runtime_bus_bridge.cpp.
extern "C" unsigned long long g_runtime_vblank_starts;

namespace gbarecomp {

// Install the active bus pointer. Subsequent bus_read_u*/bus_write_u*
// calls from generated code (declared in src/armv4t/runtime_arm.h)
// will delegate to this bus.
void set_active_bus(gba::GbaBus* bus);
void set_active_ppu(gba::GbaPpu* ppu);

// Retrieve the currently-bound bus, or nullptr if none.
gba::GbaBus* active_bus();

}  // namespace gbarecomp
