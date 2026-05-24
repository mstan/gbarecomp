// tests/codegen/stubs.h — exposes the host-side glue that
// stubs.cpp provides to the test runner.

#pragma once

#include <cstdint>

namespace codegen_test {

// Re-seed the singleton bus before each test. `size` bytes starting
// at `base`. Also resets dispatch / SWI / unimplemented recorders.
void bus_reset(uint32_t base, uint32_t size);

// Bypass the C-ABI wrappers when seeding initial memory or reading
// final memory from the runner. (The runner shouldn't go through
// bus_read_u32/bus_write_u32 because those flip oob bookkeeping.)
void     bus_write_u32_direct(uint32_t addr, uint32_t v);
uint32_t bus_read_u32_direct(uint32_t addr);

bool     bus_oob_seen();
uint32_t bus_base();
uint32_t bus_size();
const uint8_t* bus_data();

// Filled in by the strong overrides of runtime_dispatch_miss / swi.
// The runner reads these after each test to assert branch behavior.
extern uint32_t g_last_dispatch_target;
extern bool     g_dispatch_called;
extern uint32_t g_last_swi_imm;
extern bool     g_swi_called;
extern bool     g_unimplemented_called;
extern const char* g_unimplemented_op;
extern uint32_t g_unimplemented_pc;

}  // namespace codegen_test
