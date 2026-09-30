// gba_waitstates.cpp — see gba_waitstates.h.

#include "gba_waitstates.h"
#include "runtime_wait_model.h"

#include <cstring>
#include <initializer_list>

namespace gba {

namespace {

// Power-on tables, indexed by addr >> 24 (mGBA memory.c GBA_BASE_WAITSTATES,
// _32, _SEQ, _SEQ_32; index 15 is the SRAM mirror, filled by WAITCNT).
constexpr uint8_t kBaseN16[16] = {0, 0, 2, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4, 0};
constexpr uint8_t kBaseN32[16] = {0, 0, 5, 0, 0, 1, 1, 0, 7, 7, 9, 9, 13, 13, 9, 0};
constexpr uint8_t kBaseS16[16] = {0, 0, 2, 0, 0, 0, 0, 0, 2, 2, 4, 4, 8, 8, 4, 0};
constexpr uint8_t kBaseS32[16] = {0, 0, 5, 0, 0, 1, 1, 0, 5, 5, 9, 9, 17, 17, 9, 0};

// WAITCNT field decodes (GBATEK: first access 4/3/2/8, second access per
// wait-state window 2/1, 4/1, 8/1).
constexpr uint8_t kRomN[4] = {4, 3, 2, 8};
constexpr uint8_t kRomS[6] = {2, 1, 4, 1, 8, 1};

constexpr uint32_t kRegionEwram = 0x2u;
constexpr uint32_t kRegionSram = 0xEu;
constexpr uint32_t kRegionSramMirror = 0xFu;

void set_rom_window(RuntimeWaitTable& t, uint32_t first_region, uint8_t n,
                    uint8_t s) {
    for (uint32_t r = first_region; r < first_region + 2u; ++r) {
        t.n16[r] = n;
        t.s16[r] = s;
        t.n32[r] = static_cast<uint8_t>(n + 1u + s);
        t.s32[r] = static_cast<uint8_t>(2u * s + 1u);
    }
}

}  // namespace

void waits_reset(RuntimeWaitTable& t) {
    std::memcpy(t.n16, kBaseN16, sizeof(t.n16));
    std::memcpy(t.n32, kBaseN32, sizeof(t.n32));
    std::memcpy(t.s16, kBaseS16, sizeof(t.s16));
    std::memcpy(t.s32, kBaseS32, sizeof(t.s32));
    t.prefetch = 0u;
    t.last_prefetched_pc = 0u;
    waits_apply_waitcnt(t, 0u);
    waits_apply_memctl(t, 0x0D000020u);
}

void waits_apply_waitcnt(RuntimeWaitTable& t, uint16_t waitcnt) {
    const uint32_t sram = waitcnt & 0x3u;
    const uint32_t ws0 = (waitcnt >> 2) & 0x3u;
    const uint32_t ws0seq = (waitcnt >> 4) & 0x1u;
    const uint32_t ws1 = (waitcnt >> 5) & 0x3u;
    const uint32_t ws1seq = (waitcnt >> 7) & 0x1u;
    const uint32_t ws2 = (waitcnt >> 8) & 0x3u;
    const uint32_t ws2seq = (waitcnt >> 10) & 0x1u;

    for (uint32_t r : {kRegionSram, kRegionSramMirror}) {
        t.n16[r] = t.s16[r] = kRomN[sram];
        t.n32[r] = t.s32[r] = static_cast<uint8_t>(2u * kRomN[sram] + 1u);
    }
    set_rom_window(t, 0x8u, kRomN[ws0], kRomS[ws0seq]);
    set_rom_window(t, 0xAu, kRomN[ws1], kRomS[ws1seq + 2u]);
    set_rom_window(t, 0xCu, kRomN[ws2], kRomS[ws2seq + 4u]);
    t.prefetch = (waitcnt & 0x4000u) ? 1u : 0u;
}

bool waits_apply_memctl(RuntimeWaitTable& t, uint32_t memctl) {
    const uint32_t wait = 15u - ((memctl >> 24) & 0xFu);
    if (wait == 0u) return false;
    t.n16[kRegionEwram] = t.s16[kRegionEwram] = static_cast<uint8_t>(wait);
    t.n32[kRegionEwram] = t.s32[kRegionEwram] =
        static_cast<uint8_t>(2u * wait + 1u);
    return true;
}

uint32_t waits_access_cycles(const RuntimeWaitTable& t, uint32_t addr,
                             uint8_t width, bool sequential) {
    const uint32_t region = addr >> 24;
    if (region > 0xFu) return 1u;
    if (region == kRegionSram || region == kRegionSramMirror)
        return 1u + t.n16[region];
    if (width == 4u)
        return 1u + (sequential ? t.s32[region] : t.n32[region]);
    return 1u + (sequential ? t.s16[region] : t.n16[region]);
}

uint32_t waits_code(const RuntimeWaitTable& t, uint32_t pc, bool thumb,
                    bool sequential) {
    return rwt_code_wait(&t, pc, thumb ? 1u : 0u, sequential ? 1u : 0u);
}

int32_t waits_prefetch_stall(RuntimeWaitTable& t, int32_t wait,
                             uint32_t insn_pc, bool thumb) {
    return rwt_prefetch_stall(&t, wait, insn_pc, thumb ? 1u : 0u);
}

}  // namespace gba
