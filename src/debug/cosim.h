// cosim.h — GBA first-divergence co-simulation engine hooks (GBA_COSIM build).
// See COSIM_ORACLE.md. These are plain free functions so the runtime_tick hook
// site (runtime_bus_bridge.cpp) and startup (runtime.cpp) can call them without
// pulling in engine internals.
#pragma once

// Start the oracle TCP server + read stride/port/start-cycle from env. Call once
// at startup, before the guest runs an instruction.
void cosim_init(void);

// Per-instruction checkpoint hook, called at the end of runtime_tick (the shared
// master-clock advance). Records a full-state chain-hash checkpoint at each
// cycle-stride boundary and parks the guest thread there until the coordinator
// grants budget via `step`.
void cosim_on_tick(void);
