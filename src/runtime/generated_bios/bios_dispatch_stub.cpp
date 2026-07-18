#include <cstdint>

// Clean-checkout fallback used only until the user recompiles their own BIOS
// dump. The real generated dispatch table and body are selected together by
// CMake, so this stub never leaves unresolved generated function references.
struct DispatchEntry {
    std::uint32_t addr;
    std::uint8_t thumb;
    void (*fn)(void);
};

extern "C" const DispatchEntry kBiosDispatchTable[] = {
    {0u, 0u, nullptr},
};
extern "C" const unsigned kBiosDispatchTableLen = 0;
