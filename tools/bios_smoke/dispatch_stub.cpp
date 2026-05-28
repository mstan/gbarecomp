// dispatch_stub.cpp — empty cart dispatch table for bios_smoke.
//
// bios_smoke is interpreter-only: it executes via armv4t::Interpreter
// and never calls runtime_dispatch(). But it links the runtime stack,
// and runtime_arm.cpp references the cart dispatch table:
//   extern "C" const DispatchEntry kDispatchTable[];
//   extern "C" const unsigned kDispatchTableLen;
// Those symbols are normally supplied by a game's generated
// dispatch_table.cpp (e.g. MinishCapRecomp); gbarecomp standalone has
// none. Provide an empty table so the linker is satisfied. kLen=0 means
// any (never-taken) dispatch would fall through to runtime_dispatch_miss.
//
// kBiosDispatchTable is NOT defined here — bios_dispatch_table.cpp in
// gbarecomp_runtime already provides it.

#include <cstdint>

struct DispatchEntry { uint32_t addr; uint8_t thumb; void (*fn)(void); };

extern "C" const DispatchEntry kDispatchTable[1] = {
    {0xFFFFFFFFu, 0u, nullptr},
};
extern "C" const unsigned kDispatchTableLen = 0u;
