// arm_codegen.h — IR → C code emission.
//
// Stub for the scaffolding pass. The real codegen will lower each
// `Instr` into one or more C statements that operate on a `CPUState`
// and a bus interface. For now we only expose the entry point so the
// library has the symbol; the implementation returns an empty string
// and a `not_implemented` flag.

#pragma once

#include <string>
#include <vector>

#include "arm_ir.h"

namespace armv4t {

struct CodegenResult {
    std::string text;          // emitted C source for the block
    bool not_implemented;      // true if at least one Instr fell through
    std::size_t emitted_count;
};

class ArmCodegen {
public:
    static CodegenResult emit_block(const std::vector<Instr>& block);
};

}  // namespace armv4t
