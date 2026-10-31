// runtime_bus_bridge.h — public surface for binding the active bus
// to the recompiled-code runtime.

#pragma once
#include <cstdint>

struct RuntimeNativeRegion;

namespace gba { class GbaBus; }
namespace gba { class GbaPpu; }

// Count of PPU VBlank-start events (scanline 159->160), incremented in
// runtime_tick. The debug step-one-frame primitive stops on its increment
// so the recomp's TCP `step` parks at VBlank-start, matching the
// interpreter and mGBA oracles. Defined in runtime_bus_bridge.cpp.
extern "C" unsigned long long g_runtime_vblank_starts;

// Host services changing device configuration settle the previous window,
// then re-arm deadlines under the new policy, just like guest MMIO writes.
extern "C" void runtime_mmio_catch_up(void);
extern "C" void runtime_resync_horizon(void);

namespace gbarecomp {

// Install the active bus pointer. Subsequent bus_read_u*/bus_write_u*
// calls from generated code (declared in src/armv4t/runtime_arm.h)
// will delegate to this bus.
void set_active_bus(gba::GbaBus* bus);
void set_active_ppu(gba::GbaPpu* ppu);

// Retrieve the currently-bound bus / ppu, or nullptr if none.
gba::GbaBus* active_bus();
gba::GbaPpu* active_ppu();

// A whole native service may settle ordinary memory atomically only when a
// session owner or shadow validation is not executing instruction-sized slices.
bool native_service_window_allowed();

struct RuntimeTimingContext {
    std::uint64_t cycles = 0;
    std::uint64_t vblank_starts = 0;
    std::uint64_t yielded_vblank = 0;
    std::uint64_t pending_cycles = 0;
    std::int64_t event_budget = 0;
};
void runtime_capture_timing_context(RuntimeTimingContext&);
void runtime_restore_timing_context(const RuntimeTimingContext&);
// Diagnostic replay: reinstate the recorded scheduling values only. Unlike
// runtime_restore_timing_context (machine rebinding) this keeps the idle-loop
// proofs, which a replay that is in step shares with the recording.
void runtime_restore_timing_values(const RuntimeTimingContext&);
// Conservative native execution backend for the local-link qualification
// harness. CPU instructions accrue cycle debt; the session advances devices
// for every machine on one timeline. No machine's hardware runs ahead.
void runtime_session_execution(bool enabled);
void runtime_session_begin_instruction();
void runtime_session_begin_slice(std::uint64_t deadline);
void runtime_session_ram_dispatch_boundary(std::uint32_t pc);
// Reviewed IRQ services may publish private RAM as one native operation. The
// caller first rendezvous through runtime_session_ram_dispatch_boundary(),
// proves every memory range/alias and supplies the complete emulated cost.
// No MMIO, guest callback, unmasked IRQ or CPU-observing provider is permitted.
// PPU/timer boundaries still end the window; serial events can only set a
// pending IRQ until this masked service returns. Other CPUs keep running.
bool runtime_session_private_memory_begin(RuntimeNativeRegion&, std::uint32_t cycles);
void runtime_session_private_memory_end(RuntimeNativeRegion&);
// Trusted session wiring: both the DMA bus and serial endpoint must still be
// the instance-owned implementations. Reset when session execution ends.
void runtime_session_private_memory_owner(const void* owner,
    bool (*check)(const void*, const gba::GbaBus&));
void runtime_session_tick_devices(std::uint32_t cycles);
// Advance a callback-free interval with explicit device/timing ownership.
// The session must first verify its owned DMA bus and serial endpoint and
// exclude per-program read observers. Returns false without side effects at
// render/event boundaries or other observation surfaces requiring full ARM
// binding. Diagnostic PC/cycle stamps remain those of this device's CPU.
bool runtime_session_tick_owned_devices(gba::GbaBus&, gba::GbaPpu&,
    RuntimeTimingContext&, std::uint32_t pc, std::uint32_t cycles);

}  // namespace gbarecomp
