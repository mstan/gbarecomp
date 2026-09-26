// runtime_bus_bridge.h — public surface for binding the active bus
// to the recompiled-code runtime.

#pragma once
#include <cstdint>

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

// Retrieve the currently-bound bus / ppu, or nullptr if none.
gba::GbaBus* active_bus();
gba::GbaPpu* active_ppu();

struct RuntimeTimingContext {
    std::uint64_t cycles = 0;
    std::uint64_t vblank_starts = 0;
    std::uint64_t yielded_vblank = 0;
    std::uint64_t pending_cycles = 0;
    std::int64_t event_budget = 0;
};
void runtime_capture_timing_context(RuntimeTimingContext&);
void runtime_restore_timing_context(const RuntimeTimingContext&);
// Conservative native execution backend for the local-link qualification
// harness. CPU instructions accrue cycle debt; the session advances devices
// for every machine on one timeline. No machine's hardware runs ahead.
void runtime_session_execution(bool enabled);
void runtime_session_begin_instruction();
void runtime_session_begin_slice(std::uint64_t deadline);
void runtime_session_ram_dispatch_boundary(std::uint32_t pc);
void runtime_session_tick_devices(std::uint32_t cycles);

}  // namespace gbarecomp
