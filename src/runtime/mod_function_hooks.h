// mod_function_hooks.h -- trusted callbacks at reviewed GBA function entries.
//
// A [[mod_function_hook]] declaration makes code generation emit an entry
// guard for that exact (PC, instruction-set) pair.  Registered callbacks start
// disabled and may decline an invocation; the first enabled callback that
// returns non-zero owns the call.  It must leave `cpu` in the state in which
// the guest caller should resume (including R15).  The generated function then
// returns without executing its original body.
//
// The registry is deliberately always available, independent of the optional
// package catalog: generated static bodies and self-healed overlays must link
// in mod-disabled games too.
// Registration is constructor-time only and rejects unaligned addresses.
// Enable/disable and entry dispatch use
// atomics so a launcher/game-thread activation transition cannot race a call;
// an invocation already in a callback is allowed to finish.
#pragma once

#include <stdint.h>

#include "runtime_arm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*GBAModFunctionEntryCallback)(uint32_t addr, int thumb,
                                           ArmCpuState* cpu);

int gba_mod_register_function_entry_plugin(const char* id, uint32_t addr,
                                           int thumb,
                                           GBAModFunctionEntryCallback cb);
int gba_mod_set_function_hook_enabled(const char* id, int enabled);
void gba_mod_disable_all_function_hooks(void);
int gba_mod_function_hook_enabled(const char* id);

// Called only by generated/interpretive entry paths. Non-zero means the
// callback handled the call and the original guest body must be skipped. A
// callback that returns zero has all of its ArmCpuState changes discarded.
int gba_mod_function_entry(uint32_t addr, int thumb, ArmCpuState* cpu);
uint64_t gba_mod_function_hook_hits(void);

#ifdef __cplusplus
}
#endif
