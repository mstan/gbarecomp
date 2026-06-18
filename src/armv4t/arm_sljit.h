// arm_sljit.h — IR -> host machine code emission via sljit.
//
// The Tier-2 in-process JIT counterpart to arm_codegen.h (IR -> C). It consumes
// the SAME armv4t::Instr stream and must compute bit-identically to both the C
// codegen and the interpreter oracle (validated op-by-op, INCLUDING cycles, by
// the L1 JIT harness — see tests/codegen/sljit_test_main.cpp).
//
// Unlike the gcc overlay DLL, an sljit shard runs IN-PROCESS, so it reaches host
// state by baking the addresses of the runtime ABI symbols (runtime_arm.h:
// g_cpu, bus_*, arm_*, runtime_tick) directly as immediates — no callbacks
// indirection. v1 re-JITs each session, so baked absolute addresses are always
// current; v2 (serialized blobs) will switch to a cpu-relative table.
//
// SAFETY CONTRACT — precision over recall (PRINCIPLES.md "Honest self-healing"):
// the emitter lowers ONLY shapes it can prove identical to the interpreter. On
// ANY unsupported op/operand/condition it declines the WHOLE function (fn=null)
// and the caller falls to the gcc producer or the interpreter bridge. A partial
// emitter is therefore always safe: it can decline, never mis-compile. A
// mis-compiled shard is fatal; a declined function is free.

#pragma once

#include <cstdint>

#include "arm_ir.h"

namespace armv4t {

// A compiled single-instruction shard. `fn` is the entry (a `void fn(void)`
// operating on the baked-in g_cpu + ABI); null means the emitter DECLINED this
// instruction (unsupported shape). `code`/`code_size` are the sljit-generated
// block, owned by the caller (free via free_sljit_fn).
struct SljitFn {
    void (*fn)(void) = nullptr;
    void* code = nullptr;
    unsigned long code_size = 0;
};

// JIT one decoded instruction into a standalone host function. This is the
// per-instruction leaf the L1 harness validates (parallel to the C codegen's
// per-case tc_<idx>); the whole-function producer (P5) composes leaves with
// control flow. Returns fn=null when the op/shape is not yet lowered.
SljitFn emit_instr_sljit(const Instr& ins);

// Release a compiled shard (sljit_free_code). Safe on a declined (null) result.
void free_sljit_fn(const SljitFn& f);

// True if the emitter would attempt this instruction (vs. decline). Lets the
// harness distinguish "declined, expected" from "compiled but wrong". Pure;
// must exactly predict whether emit_instr_sljit returns non-null.
bool sljit_supports(const Instr& ins);

}  // namespace armv4t
