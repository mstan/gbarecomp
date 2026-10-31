#include "runtime_arm.h"
#include "gba_bus.h"
#include "runtime_bus_bridge.h"
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
gba::GbaBus* active;
unsigned calls;
uint32_t random_state = 0x91B2C3D4u;
uint32_t next() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    return random_state ^= random_state << 5;
}
const uint8_t* view(uint32_t pc, uint32_t size) {
    ++calls;
    return active ? active->native_code_view(pc, size) : nullptr;
}
const uint8_t* other_view(uint32_t pc, uint32_t size) {
    ++calls;
    return active ? active->native_code_view(pc, size) : nullptr;
}
void require(bool condition, const char* label) {
    if (!condition) { std::fprintf(stderr, "%s\n", label); std::abort(); }
}
void publish(gba::GbaBus& bus) {
    active = &bus;
    g_runtime_ram_code_view = view;
    g_runtime_native_ram_code_source = {view, bus.ewram_ptr(), bus.iwram_ptr()};
}
}

int main() {
    gba::GbaBus a, b;
#if defined(GBARECOMP_NATIVE_RAM_CODE_VIEWS)
    gbarecomp::set_active_bus(&a);
    require(g_runtime_native_ram_code_source.view == g_runtime_ram_code_view &&
        g_runtime_native_ram_code_source.ewram == a.ewram_ptr() &&
        g_runtime_native_ram_code_source.iwram == a.iwram_ptr(), "Runtime owner publication missing");
    const RuntimeNativeRamCodeView owner_view;
    gbarecomp::set_active_bus(&b);
    b.iwram_ptr()[0] = 0x12; b.iwram_ptr()[1] = 0x34;
    require(owner_view.matches(0x03000000u, 0x3412u, 2u), "Runtime owner rebind uses stale bytes");
    gbarecomp::set_active_bus(nullptr);
    require(!g_runtime_native_ram_code_source.ewram && !g_runtime_native_ram_code_source.iwram &&
        !owner_view.matches(0x03000000u, 0, 2u), "Null runtime owner remains readable");
#endif
    for (uint32_t n = 0; n < 0x40000u; ++n) {
        a.ewram_ptr()[n] = uint8_t(next());
        b.ewram_ptr()[n] = uint8_t(next());
    }
    for (uint32_t n = 0; n < 0x8000u; ++n) {
        a.iwram_ptr()[n] = uint8_t(next());
        b.iwram_ptr()[n] = uint8_t(next());
    }
    publish(a);
    const RuntimeNativeRamCodeView borrowed;
    uint64_t comparisons = 0, bypassed_views = 0;
    for (unsigned trial = 0; trial < 32768; ++trial) {
        const uint32_t region = trial & 1u ? 0x02000000u : 0x03000000u;
        const uint32_t mask = region == 0x02000000u ? 0x3FFFFu : 0x7FFFu;
        const uint32_t size = trial & 2u ? 2u : 4u;
        const uint32_t pc = region | (next() & 0x00FFFFFFu);
        const uint32_t opcode = next(), compare_mask = trial & 4u ? next() : 0xFFFFFFFFu;
        const int expected = runtime_native_ram_instruction_matches_masked(pc, opcode, size, compare_mask);
        calls = 0;
        require(borrowed.matches(pc, opcode, size, compare_mask) == expected, "Live/mirrored instruction mismatch");
        const bool wraps = (pc & mask) > mask + 1u - size;
        require(calls == unsigned(wraps), "Expected one maintained callback only for wrapped fragment");
        bypassed_views += !wraps;
        ++comparisons;

        // Native/mod/debug writes need no cache invalidation: the same lease
        // must observe changed memory on its very next instruction.
        auto* data = region == 0x02000000u ? a.ewram_ptr() : a.iwram_ptr();
        data[pc & mask] ^= uint8_t(1u << (trial & 7u));
        const int changed = runtime_native_ram_instruction_matches_masked(pc, opcode, size, compare_mask);
        require(borrowed.matches(pc, opcode, size, compare_mask) == changed, "Direct self-modifying write mismatch");
        ++comparisons;
    }
    // Known true and false guards, plus signed/masked immediate data, are not
    // adequately covered by random expected-opcode mismatches alone.
    for (uint32_t encoded = 0; encoded < 4096u; ++encoded) {
        auto* p = a.iwram_ptr() + 0x7FFCu;
        const uint32_t opcode = 0xE3A00000u | encoded;
        for (unsigned n=0;n<4;++n) p[n] = uint8_t(opcode >> (n*8));
        require(borrowed.matches(0x03FFFFFCu, opcode, 4u), "Exact opcode declined");
        require(!borrowed.matches(0x03FFFFFCu, opcode ^ 0x10000000u, 4u), "Changed fixed opcode accepted");
        require(borrowed.matches(0x03FFFFFCu, opcode ^ 0xFFFu, 4u, 0xFFFFF000u), "Dynamic immediate fixed-bit guard mismatch");
        const uint32_t expected = std::rotr(encoded & 255u, int(((encoded >> 8) & 15u)*2u));
        calls = 0;
        require(borrowed.arm_immediate(0x03FFFFFCu) == expected && calls == 0, "Native immediate decode mismatch");
        require(runtime_native_arm_immediate(0x03FFFFFCu) == expected, "Maintained immediate oracle mismatch");
        comparisons += 5;
    }
    for (uint32_t region : {0x02000000u, 0x03000000u}) {
        const uint32_t end = region | 0x00FFFFFFu;
        for (uint32_t width : {2u, 4u}) {
            for (uint32_t back=0;back<4;++back) {
                const uint32_t pc = end-back;
                const uint32_t expected = runtime_native_ram_instruction_matches(pc, next(), width);
                // Explicit region-end overflow must follow the old refusal,
                // even though the physical RAM itself is mirrored.
                if (uint64_t(pc)+width > uint64_t(region)+0x01000000ull)
                    require(!expected && !borrowed.matches(pc, 0, width), "Region-end overflow accepted");
            }
        }
    }
    publish(b);
    for (uint32_t size : {2u,4u}) {
        calls = 0;
        const uint32_t opcode = next();
        const int actual = borrowed.matches(0x02000018u, opcode, size);
        require(calls == 1, "Rebound owner must use maintained view");
        require(actual == runtime_native_ram_instruction_matches(0x02000018u, opcode, size), "Rebound owner mismatch");
        ++comparisons;
    }
    publish(a);
    g_runtime_ram_code_view = other_view;
    calls = 0;
    const int custom = borrowed.matches(0x03000018u, 0, 4u);
    require(calls == 1 && custom == runtime_native_ram_instruction_matches(0x03000018u, 0, 4u), "Changed callback contract lost");
    calls = 0;
    const uint32_t immediate = borrowed.arm_immediate(0x03000018u);
    require(calls == 1 && immediate == runtime_native_arm_immediate(0x03000018u), "Changed immediate callback contract lost");
    g_runtime_native_ram_code_source = {};
    calls = 0;
    borrowed.matches(0x03000018u,0,4u);
    require(calls == 1, "Revoked publication must use maintained callback");
    g_runtime_ram_code_view = nullptr;
    require(!borrowed.matches(0x03000018u,0,4u), "Null callback accepted");
    for (uint32_t size : {0u,1u,3u,5u})
        require(!borrowed.matches(0x03000018u,0,size), "Invalid instruction width accepted");
    publish(a);
    calls = 0;
    borrowed.matches(0x03000018u,0,4u);
    require(!calls, "Restored exact binding cannot reuse live view");
    g_runtime_ram_code_view = nullptr;
    g_runtime_native_ram_code_source = {};
    std::printf("Live RAM view: %llu comparisons; %llu known-view calls bypassed; live writes, mirrors, all4096 immediates, rebind/revoke/custom/null/overflow guards pass\n",
                static_cast<unsigned long long>(comparisons), static_cast<unsigned long long>(bypassed_views));
}
