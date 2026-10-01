// timing_test â€” runs the recompiled CPU timing fixture (timing_fixture.S)
// and reports its timer-measured kernel cycles.
//
//   timing_rom_tests <rom> [--csv]
//
// Direct boot like the mGBA oracle (no BIOS, PC = 08000000h, power-on
// WAITCNT), dispatching the statically recompiled fixture until it writes
// its done marker. --csv prints "index,cycles" for every measurement (the
// oracle/timing/compare.py differential against the pinned mGBA). Without
// it, the self-test checks the marker plus a set of values derived by hand
// from GBATEK cycle timings (independent of mGBA), and that the run was
// fully static.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "self_heal.h"

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)

namespace {

constexpr uint32_t kResults = 0x02020000u;
constexpr uint32_t kMarker = 0x0201FFF0u;
constexpr uint32_t kMarkerDone = 0x600Du;
constexpr unsigned kPasses = 5, kRegions = 3, kKernels = 33;

// Index of a measurement: pass (WAITCNT setting), region (0 ROM, 1 IWRAM,
// 2 EWRAM), kernel (timing_fixture.S kernel_table order).
constexpr unsigned idx(unsigned pass, unsigned region, unsigned kernel) {
    return (pass * kRegions + region) * kKernels + kernel;
}

struct Expected { unsigned index; uint32_t cycles; const char* why; };

// Hand-derived from GBATEK "GBA Memory Map" / "ARM CPU Instruction Cycle
// Times" and the GamePak prefetch description, independent of mGBA. Power-on
// ROM WS0 is N=4/S=2 wait states on a 16-bit bus (ARM fetch 1+5, N32 1+7;
// THUMB fetch 1+2, N16 1+4); WAITCNT 4317h gives ROM N=3/S=1. IWRAM/IO are
// zero-wait 32-bit, EWRAM 2 waits on a 16-bit bus. Each value spans the
// first timer LDRH (fetch, IO read + internal cycle, next fetch N) and the
// workload. The mGBA differential checks every entry; these pin the model.
const Expected kExpected[] = {
    {idx(0, 0, 0), 58, "ARM ROM ALU: ldrh 6+2+2, 8 x 6"},
    {idx(0, 0, 1), 204, "ARM ROM loop: ldrh 10, 7 x (subs 6 + bne 20), 6+6"},
    {idx(0, 0, 2), 50, "ARM ROM ldr IWRAM: ldrh 10, 4 x (6+1+1+2)"},
    {idx(0, 0, 19), 31, "THUMB ROM ALU: ldrh 3+2+2, 8 x 3"},
    {idx(0, 1, 0), 11, "ARM IWRAM ALU: ldrh 3, 8 x 1"},
    {idx(0, 1, 1), 33, "ARM IWRAM loop: ldrh 3, 7 x (1 + 3), 1+1"},
    {idx(0, 1, 2), 15, "ARM IWRAM ldr IWRAM: ldrh 3, 4 x 3"},
    {idx(0, 1, 19), 11, "THUMB IWRAM ALU: ldrh 3, 8 x 1"},
    {idx(0, 2, 0), 56, "ARM EWRAM ALU: ldrh 6+2, 8 x 6"},
    {idx(0, 2, 19), 29, "THUMB EWRAM ALU: ldrh 3+2, 8 x 3"},
    {idx(1, 0, 0), 36, "ARM ROM 3/1+prefetch ALU: ldrh 4-2+2, 8 x 4"},
    {idx(1, 0, 2), 20, "ARM ROM 3/1+prefetch ldr IWRAM: 5 x (4-2+2)"},
    {idx(1, 0, 19), 18, "THUMB ROM 3/1+prefetch ALU: ldrh 2-2+2, 8 x 2"},
};

bool read_file(const char* path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return !out.empty();
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc == 2 || argc == 3);
    const bool csv = argc == 3 && std::strcmp(argv[2], "--csv") == 0;
    CHECK(argc == 2 || csv);
    std::vector<uint8_t> rom;
    CHECK(read_file(argv[1], rom));

    gba::GbaBus bus;
    gba::GbaPpu ppu;
    bus.set_rom(rom.data(), rom.size());
    bus.io().set_ppu(&ppu);
    bus.io().set_bus(&bus);
    gbarecomp::set_active_bus(&bus);
    gbarecomp::set_active_ppu(&ppu);
    runtime_init(&bus);

    for (int i = 0; i < 16; ++i) g_cpu.R[i] = 0;
    g_cpu.R[15] = 0x08000000u;
    g_cpu.cpsr = CPSR_I_BIT | CPSR_F_BIT | 0x1Fu;  // System, like mGBA's skip

    // Frames are 280896 cycles; the fixture needs well under one second.
    for (unsigned step = 0; step < 50'000'000u; ++step) {
        if (bus.read32(kMarker) == kMarkerDone) break;
        runtime_dispatch(g_cpu.R[15]);
    }
    CHECK(bus.read32(kMarker) == kMarkerDone);

    std::vector<uint32_t> values;
    for (unsigned i = 0; i < kPasses * kRegions * kKernels; ++i)
        values.push_back(bus.read32(kResults + 4u * i));
    CHECK(bus.read32(kResults + 4u * values.size()) == 0u);

    if (csv) {
        for (unsigned i = 0; i < values.size(); ++i)
            std::printf("%u,%u\n", i, values[i]);
        return 0;
    }
    int failures = 0;
    for (const auto& e : kExpected) {
        if (values[e.index] != e.cycles) {
            std::fprintf(stderr, "timing[%u] = %u, expected %u (%s)\n",
                         e.index, values[e.index], e.cycles, e.why);
            ++failures;
        }
    }
    CHECK(!gbarecomp::self_heal_any_misses());
    CHECK(failures == 0);
    std::printf("timing fixture: %zu measurements, hand-derived anchors and "
                "static coverage passed\n", values.size());
    return 0;
}
