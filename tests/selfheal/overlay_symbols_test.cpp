// The Stage-2 overlay shim must expose every host value the generated bodies
// read. A missing name fails at the overlay compiler, after the game has
// already bridged the PC, so keep the contract checkable at build time.
#include <cstdint>
#include <type_traits>

#include "overlay_abi.h"
#include "overlay_runtime_arm.h"

static_assert(GBA_OVERLAY_ABI_VERSION >= 7u,
              "generated bodies need the ABI v7 additions");
static_assert(std::is_same_v<decltype(GbaOverlayCallbacks::runtime_resume_pc),
                             uint32_t*>);
static_assert(std::is_same_v<decltype(GbaOverlayCallbacks::runtime_native_ram_base),
                             uint32_t*>);

namespace {

// Never called: compiling this proves the shim names resolve for generated
// code, which is the failure this test exists to catch.
void shim_names_compile() {
    g_runtime_resume_pc = 0;
    g_runtime_native_ram_base = 0;
}

}  // namespace

int main() { return 0; }
