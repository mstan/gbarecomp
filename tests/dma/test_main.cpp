#include <array>
#include <cstdint>
#include <cstdio>

#include "gba_io.h"

namespace {

class TestBus final : public armv4t::Bus {
public:
    uint8_t read8(uint32_t addr) override { return byte(addr); }
    uint16_t read16(uint32_t addr) override {
        return static_cast<uint16_t>(read8(addr) | (read8(addr + 1) << 8));
    }
    uint32_t read32(uint32_t addr) override {
        return static_cast<uint32_t>(read8(addr)) |
               (static_cast<uint32_t>(read8(addr + 1)) << 8) |
               (static_cast<uint32_t>(read8(addr + 2)) << 16) |
               (static_cast<uint32_t>(read8(addr + 3)) << 24);
    }
    void write8(uint32_t addr, uint8_t value) override { byte(addr) = value; }
    void write16(uint32_t addr, uint16_t value) override {
        write8(addr, static_cast<uint8_t>(value));
        write8(addr + 1, static_cast<uint8_t>(value >> 8));
    }
    void write32(uint32_t addr, uint32_t value) override {
        write8(addr, static_cast<uint8_t>(value));
        write8(addr + 1, static_cast<uint8_t>(value >> 8));
        write8(addr + 2, static_cast<uint8_t>(value >> 16));
        write8(addr + 3, static_cast<uint8_t>(value >> 24));
    }

private:
    uint8_t& byte(uint32_t addr) {
        if ((addr & 0xFF000000u) == 0x02000000u) {
            return ewram_[addr & (ewram_.size() - 1)];
        }
        return iwram_[addr & (iwram_.size() - 1)];
    }

    std::array<uint8_t, 256 * 1024> ewram_{};
    std::array<uint8_t, 32 * 1024> iwram_{};
};

int failures = 0;

void check_eq(const char* test, uint32_t got, uint32_t expected) {
    if (got != expected) {
        std::printf("FAIL %s: got 0x%08X, expected 0x%08X\n",
                    test, got, expected);
        ++failures;
    }
}

void configure_dma(gba::GbaIo& io, uint32_t source, uint32_t dest,
                   uint16_t count, uint16_t control) {
    io.write32(0xB0, source);
    io.write32(0xB4, dest);
    io.write16(0xB8, count);
    io.write16(0xBA, control);
}

void test_immediate_word_alignment() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write32(0x02000000u, 0x78563412u);
    bus.write32(0x02000004u, 0xAABBCCDDu);

    configure_dma(io, 0x02000001u, 0x03000003u, 1, 0x8400u);

    check_eq("immediate_word_aligned_dest", bus.read32(0x03000000u),
             0x78563412u);
    check_eq("immediate_word_no_unaligned_write", bus.read32(0x03000004u), 0u);
    check_eq("immediate_word_run_count", static_cast<uint32_t>(io.dma_runs(0)), 1u);
    check_eq("immediate_word_unit_count", static_cast<uint32_t>(io.dma_words(0)), 1u);
}

void test_immediate_halfword_alignment() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write16(0x02000010u, 0xBEEFu);
    bus.write16(0x02000012u, 0x1234u);

    configure_dma(io, 0x02000011u, 0x03000021u, 1, 0x8000u);

    check_eq("immediate_halfword_aligned_dest", bus.read16(0x03000020u),
             0xBEEFu);
    check_eq("immediate_halfword_no_unaligned_write", bus.read16(0x03000022u), 0u);
}

void test_timed_word_alignment() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write32(0x02000040u, 0xDEADBEEFu);
    bus.write32(0x02000044u, 0x01020304u);

    configure_dma(io, 0x02000041u, 0x03000043u, 1, 0x9400u);
    check_eq("timed_word_not_immediate", bus.read32(0x03000040u), 0u);
    io.run_timed_dma(1);

    check_eq("timed_word_aligned_dest", bus.read32(0x03000040u), 0xDEADBEEFu);
    check_eq("timed_word_no_unaligned_write", bus.read32(0x03000044u), 0u);
}

}  // namespace

int main() {
    test_immediate_word_alignment();
    test_immediate_halfword_alignment();
    test_timed_word_alignment();
    if (failures) {
        std::printf("dma_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("dma_tests: OK\n");
    return 0;
}
