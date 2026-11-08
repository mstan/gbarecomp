// The Stage-2 overlay shim must expose every host value the generated bodies
// read. A missing name fails at the overlay compiler, after the game has
// already bridged the PC, so keep the contract checkable at build time.
#include <cstdint>
#include <cstdio>
#include <type_traits>

#include "overlay_abi.h"
#include "overlay_runtime_arm.h"

static_assert(GBA_OVERLAY_ABI_VERSION >= 7u,
              "generated bodies need the ABI v7 additions");
static_assert(std::is_same_v<decltype(GbaOverlayCallbacks::runtime_resume_pc),
                             uint32_t*>);
static_assert(std::is_same_v<decltype(GbaOverlayCallbacks::runtime_native_ram_base),
                             uint32_t*>);

const GbaOverlayCallbacks* g_ovl = nullptr;

int main() {
    static uint32_t resume_pc = 0x08000100u;
    static uint32_t ram_base = 0x02000000u;
    static GbaOverlayCallbacks callbacks{};
    callbacks.abi_version = GBA_OVERLAY_ABI_VERSION;
    callbacks.runtime_resume_pc = &resume_pc;
    callbacks.runtime_native_ram_base = &ram_base;
    g_ovl = &callbacks;

    if (g_runtime_resume_pc != resume_pc ||
        g_runtime_native_ram_base != ram_base) {
        std::fputs("overlay shim did not read host storage\n", stderr);
        return 1;
    }
    g_runtime_resume_pc = 0x08000104u;
    g_runtime_native_ram_base = 0x03000000u;
    if (resume_pc != 0x08000104u || ram_base != 0x03000000u) {
        std::fputs("overlay shim did not publish to host storage\n", stderr);
        return 1;
    }
    resume_pc = 0x08000108u;
    ram_base = 0x02000000u;
    if (g_runtime_resume_pc != 0x08000108u ||
        g_runtime_native_ram_base != 0x02000000u) {
        std::fputs("overlay shim copied rather than aliased host storage\n", stderr);
        return 1;
    }
    return 0;
}
