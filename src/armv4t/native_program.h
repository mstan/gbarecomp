#pragma once
#include <cstdint>

// Generated immutable code, including copied-RAM routines, belongs to one
// exact cartridge image. A profile never falls through to another cart table.
struct DispatchEntry {
    uint32_t addr;
    uint8_t thumb;
    uint8_t resume;
    void (*fn)(void);
};
struct RuntimeNativeRamTemplate;
struct RuntimeNativeRamEntry;
struct RuntimeNativeProgram {
    const DispatchEntry* dispatch;
    uint32_t dispatch_count;
    const RuntimeNativeRamTemplate* ram_templates;
    uint32_t ram_template_count;
    const RuntimeNativeRamEntry* ram_entries;
    uint32_t ram_entry_count;
};

// Null restores the executable's original cart and RAM-template tables.
// Called once before publishing the cartridge or constructing machine state.
extern "C" void runtime_select_native_program(const RuntimeNativeProgram* program);
