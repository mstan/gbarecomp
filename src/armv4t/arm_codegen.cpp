#include "arm_codegen.h"

namespace armv4t {

CodegenResult ArmCodegen::emit_block(const std::vector<Instr>& block) {
    CodegenResult r{};
    r.not_implemented = true;
    r.emitted_count = block.size();
    // The real lowering lives in a follow-up phase. For the smoke pass
    // we deliberately keep the entry point trivial so dependent tools
    // can link without pulling in the not-yet-written lowering.
    return r;
}

}  // namespace armv4t
