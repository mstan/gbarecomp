// tests/codegen/test_cases.h — shared between the generator and the
// runner. Defines the per-instruction L1 diff test corpus.
//
// L1 testing answers "does ArmCodegen::emit_instr produce C code that
// matches Interpreter::step on a synthesized input?". The harness:
//   1. The generator (gen_codegen_tests.exe) consumes this list, runs
//      the decoder + ArmCodegen, emits one `tc_<idx>` C function per
//      case into generated/test_funcs.cpp.
//   2. The runner (codegen_tests.exe) consumes the SAME list. For each
//      case it:
//        a. Builds CPUState + a FlatBus from the snapshot, runs the
//           interpreter once.
//        b. Builds g_cpu + a singleton FlatBus from the same snapshot,
//           invokes the generated `tc_<idx>` function.
//        c. Diffs registers, CPSR, and memory.
//
// A test case is intentionally minimal — single instruction, fully
// pinned initial state, no surrounding control flow. The whole point
// is to catch divergence at the per-IrOp level, BEFORE it compounds
// in a 16 KB BIOS workload.

#pragma once

#include <cstddef>
#include <cstdint>

struct TestMemInit {
    uint32_t addr;
    uint32_t value;
};

struct TestCase {
    const char* name;          // human-readable; printed on failure
    bool        thumb;         // ARM (false) or THUMB (true)
    uint32_t    pc;            // address of the instruction itself
    uint32_t    word;          // 32-bit ARM word OR 16-bit THUMB halfword
                               // (zero-extended into uint32_t)

    // Initial CPU state. r_init[15] is ignored for the recomp path —
    // recompiled functions don't read R[15] from the register file
    // (the decoder bakes the PC-pipeline value in as an immediate).
    // The interpreter, by contrast, does read R[15]; the runner sets
    // it to `pc` before stepping.
    uint32_t    r_init[16];
    uint32_t    cpsr_init;     // packed CPSR (matches ArmCpuState)

    // Sparse word-grained memory init. Each entry writes a u32 LE to
    // the bus at the given address before the test runs.
    const TestMemInit* mem_init;
    size_t             mem_init_count;

    // Recomp doesn't advance PC on non-branch ops — only branches
    // write R[15]. The interpreter ALWAYS advances. To diff R[15]
    // honestly the runner restores cpu.R[15] to its pre-step value
    // when `branches` is false.
    bool        branches;

    // Memory region the test uses. The runner seeds a singleton
    // FlatBus of `mem_size` bytes at `mem_base`. For tests that
    // don't touch memory, leave both at 0 to use the defaults.
    uint32_t    mem_base;
    uint32_t    mem_size;
};

extern const TestCase kTestCases[];
extern const std::size_t kTestCasesCount;

// Emitted by generated/test_funcs.cpp. One entry per kTestCases slot.
using TestFn = void (*)(void);
extern "C" const TestFn       kTestFns[];
extern "C" const unsigned     kTestFnsCount;
