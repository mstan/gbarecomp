#include <cstdint>

// GbaIo stamps diagnostics with the runtime clock and current PC. The offline
// heal-gate test has no runtime, and its assertions do not inspect those
// diagnostic fields, so stable zero-valued target-local definitions preserve
// the test's pure layering without linking the full runtime stack.
extern "C" {
unsigned long long g_runtime_cycles = 0;

uint32_t runtime_current_pc(void) {
    return 0;
}
}
