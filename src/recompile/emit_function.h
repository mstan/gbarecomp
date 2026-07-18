// emit_function.h — lower ONE discovered function to its C body text.
//
// Extracted from tools/gba_recompile/main.cpp so the SAME emitter serves
// both the offline corpus build (the tool) AND the runtime self-healing
// recompiler (Stage 2: a dispatch miss → emit this one function → gcc → DLL).
// emit_function_body_str() returns the statements that go INSIDE the
// `void name(void) { ... }` wrapper, byte-for-byte identical to what the
// tool has always written — which is what lets a runtime-recompiled overlay
// be fingerprint-identical to the static corpus.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "function_finder.h"  // gbarecomp::Function, CpuMode

namespace gbarecomp {

// Body text for `fn` (the inside of `void <name>(void) { ... }`).
// `names_by_key` maps (addr<<1)|thumb → function name for resolving direct
// B/BL targets to C calls; an EMPTY map lowers every B/BL to
// runtime_dispatch(target) (the form Stage-2 overlays use — all inter-function
// flow routes back through the host dispatcher).
std::string emit_function_body_str(
    const Function& fn, const uint8_t* rom, std::size_t rom_size,
    uint32_t rom_base,
    const std::unordered_map<uint64_t, std::string>& names_by_key,
    const std::unordered_set<uint32_t>*
        alu_immediate_override_pcs = nullptr);

// Thin FILE* wrapper: emit_function_body_str + fputs. Used by the offline
// tool's write_body() so its output stays byte-identical.
void emit_function_body(
    std::FILE* f, const Function& fn, const uint8_t* rom,
    std::size_t rom_size, uint32_t rom_base,
    const std::unordered_map<uint64_t, std::string>& names_by_key,
    const std::unordered_set<uint32_t>*
        alu_immediate_override_pcs = nullptr);

}  // namespace gbarecomp
