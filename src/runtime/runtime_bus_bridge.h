// runtime_bus_bridge.h — public surface for binding the active bus
// to the recompiled-code runtime.

#pragma once

namespace gba { class GbaBus; }

namespace gbarecomp {

// Install the active bus pointer. Subsequent bus_read_u*/bus_write_u*
// calls from generated code (declared in src/armv4t/runtime_arm.h)
// will delegate to this bus.
void set_active_bus(gba::GbaBus* bus);

// Retrieve the currently-bound bus, or nullptr if none.
gba::GbaBus* active_bus();

}  // namespace gbarecomp
