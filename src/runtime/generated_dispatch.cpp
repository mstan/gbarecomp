// generated_dispatch.cpp — STUB.

#include "generated_dispatch.h"

#include <cstdio>

namespace gbarecomp {

void on_dispatch_miss(uint32_t addr, bool thumb) {
    std::fprintf(stderr,
                 "[gbarecomp:dispatch] MISS @ 0x%08x (%s)\n",
                 addr, thumb ? "thumb" : "arm");
    // Real path writes to dispatch_misses.log next to the executable.
    // See CLAUDE.md "DISPATCH MISS RULE".
}

}  // namespace gbarecomp
