#include "gba_bus.h"
#include "gba_io.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

static int failures;
static void check(bool yes, const char* name) {
    if (!yes) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); }
}
static void dma(gba::GbaIo& io, uint32_t src, uint32_t dst, uint16_t count, uint16_t ctl) {
    io.write32(0xB0, src); io.write32(0xB4, dst);
    io.write16(0xB8, count); io.write16(0xBA, ctl);
}
struct Observer : gba::BusWriteObserver {
    unsigned writes = 0;
    bool on_bus_write(gba::BusWriteRegion, uint32_t, uint32_t, uint8_t, uint32_t, uint32_t) override {
        ++writes; return true;
    }
};
int main(int argc, char**) {
    auto bus = std::make_unique<gba::GbaBus>();
    auto& io = bus->io();
    io.set_bus(bus.get());
    for (unsigned i = 0; i < 65536; ++i) bus->ewram_ptr()[i] = static_cast<uint8_t>(i * 37u);
    for (uint32_t width : {2u, 4u}) {
        const uint16_t ctl = static_cast<uint16_t>(0xC000u | (width == 4 ? 0x0400u : 0u));
        dma(io, 0x02000001u, 0x03000001u, 128, ctl);
        check(std::memcmp(bus->ewram_ptr(), bus->iwram_ptr(), 128 * width) == 0, "aligned immediate copy");
        check((io.read16(0xBA) & 0x8000u) == 0, "completion clears enable");
        check((io.read16(0x202) & 0x100u) != 0, "completion IRQ");
        const unsigned debt = io.take_dma_steal_cycles();
        check(debt != 0, "DMA time debt");
        std::printf("contract width=%u debt=%u count=%zu units=%zu irq=%u\n",
            width, debt, io.dma_runs(0), io.dma_words(0), io.read16(0x202));
        // HBlank repeat, increment/reload destination: two triggers each
        // publish a fresh source span to the same destination.
        dma(io, 0x02000000u, 0x03000100u, 32, static_cast<uint16_t>(0xA260u | (width == 4 ? 0x400u : 0u)));
        io.run_timed_dma(2); io.run_timed_dma(2);
        check(std::memcmp(bus->ewram_ptr() + 32 * width, bus->iwram_ptr() + 0x100, 32 * width) == 0,
              "repeat source continuation and destination reload");
        dma(io, 0x02000000u, 0x02000000u, 1, 0); // disable before subsequent checks
    }
    bus->write32(0x02010000u, 0x12345678u);
    dma(io, 0x02010000u, 0x02010004u, 16, 0x8400u);
    check(bus->read32(0x02010040u) == 0x12345678u, "forward overlap propagation");
    check(!bus->dma_copy_ram(0x0203FFF0u, 0x03000000u, 32), "physical mirror crossing rejected");
    check(!bus->dma_copy_ram(0x02000000u, 0x04000000u, 32), "MMIO not bulk RAM");
    Observer observer;
    bus->set_write_observer(&observer);
    dma(io, 0x02000000u, 0x03001000u, 16, 0x8400u);
    check(observer.writes == 16, "write observer sees each unit and can suppress");
    bus->set_write_observer(nullptr);
    if (argc > 1 && !failures) {
        constexpr unsigned transfers = 2000000;
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned bytes : {64u, 1024u, 65536u}) {
            const unsigned count = bytes == 64 ? 1500000 : (bytes == 1024 ? 437500 : 62500);
            const auto size_begin = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < count; ++i) {
            dma(io, 0x02000000u, 0x02020000u, static_cast<uint16_t>(bytes / 4), 0x8400u);
            }
            std::printf("component_size bytes=%u transfers=%u seconds=%.6f\n", bytes, count,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - size_begin).count());
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        uint64_t digest = 1469598103934665603ULL;
        for (unsigned i = 0; i < 65536; ++i) {
            const uint8_t value = bus->ewram_ptr()[0x20000 + i];
            check(value == bus->ewram_ptr()[i], "probe output bytes");
            digest = (digest ^ value) * 1099511628211ULL;
        }
        std::printf("component_probe transfers=%u sizes=64,1024,65536 counts=1500000,437500,62500 seconds=%.6f digest=%llu backend=%s\n",
            transfers, seconds, static_cast<unsigned long long>(digest), gba::dma_ram_implementation());
    }
    std::printf("dma_bulk_tests failures=%d\n", failures);
    return failures ? 1 : 0;
}
