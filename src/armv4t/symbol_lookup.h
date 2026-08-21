// symbol_lookup.h — runtime address -> recompiled-function-name lookup.
//
// Turns raw guest PCs in debug output (trace dumps, dispatch-miss reports,
// TCP) into `<FunctionName+0xoffset>`. Names come from the decomp symbols
// the recompiler seeds into generated code; the recompiler emits a sorted
// address->name table (symbol_map.cpp / bios_symbol_map.cpp) whose static
// initializer REGISTERS itself here at startup. Registration is used rather
// than weak externs because MinGW PE-COFF weak symbols don't reliably
// resolve from static archives (see runtime_arm.cpp header). A binary
// without a generated map simply never registers one and the lookup
// returns nullptr (no annotation) — graceful, and no link dependency.
#pragma once

#include <cstdint>

struct GbaSymbol { uint32_t addr; const char* name; };

// A named region of guest memory (global, buffer, MMIO register) imported
// from a decomp. Unlike a function symbol this carries its extent: guest
// memory is sparse, so "nearest entry below" is not a meaningful answer —
// the symbol 700 KB below an address says nothing about it.
struct GbaDataSymbol { uint32_t addr; uint32_t size; const char* name; };

extern "C" {

// Returns the name of the nearest recompiled function whose entry address
// is <= pc, setting *out_offset to (pc - entry_addr). Returns nullptr when
// no symbol map is registered, or when pc precedes the first known entry.
const char* gba_symbol_lookup(uint32_t pc, uint32_t* out_offset);

// Called by the generated symbol_map.cpp / bios_symbol_map.cpp static
// initializers. `tab` must be sorted ascending by addr and live for the
// program's lifetime (it is static data in the generated TU).
void gba_symbol_register_cart(const GbaSymbol* tab, unsigned count);
void gba_symbol_register_bios(const GbaSymbol* tab, unsigned count);

// Returns the name of the data symbol CONTAINING `addr`, setting
// *out_offset to (addr - symbol_addr). Returns nullptr when no data map is
// registered or the address falls in no symbol's extent — deliberately
// strict, so an unnamed address is reported as unnamed rather than
// attributed to whatever happens to sit below it.
//
// A symbol whose extent is unknown (size 0 — typical of the absolute
// symbols a part-disassembled decomp declares) resolves only for a small
// window from its start; kUnsizedWindow in the .cpp documents the bound.
const char* gba_data_symbol_lookup(uint32_t addr, uint32_t* out_offset);

// Called by the generated data_symbol_map.cpp static initializer. `tab`
// must be sorted ascending by addr, unique per address, and live for the
// program's lifetime.
void gba_data_symbol_register_cart(const GbaDataSymbol* tab, unsigned count);

}  // extern "C"
