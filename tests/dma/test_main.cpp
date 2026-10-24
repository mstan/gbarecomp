#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "gba_audio.h"
#include "gba_io.h"

namespace {

class TestBus final : public armv4t::Bus {
public:
    void set_io(gba::GbaIo* io) { io_ = io; }
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
        if (io_ && (addr & 0xFF000000u) == 0x04000000u) {
            io_->write16(addr - 0x04000000u, value);
            return;
        }
        write8(addr, static_cast<uint8_t>(value));
        write8(addr + 1, static_cast<uint8_t>(value >> 8));
    }
    void write32(uint32_t addr, uint32_t value) override {
        if (io_ && (addr & 0xFF000000u) == 0x04000000u) {
            io_->write32(addr - 0x04000000u, value);
            return;
        }
        write8(addr, static_cast<uint8_t>(value));
        write8(addr + 1, static_cast<uint8_t>(value >> 8));
        write8(addr + 2, static_cast<uint8_t>(value >> 16));
        write8(addr + 3, static_cast<uint8_t>(value >> 24));
    }

private:
    gba::GbaIo* io_ = nullptr;
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

void test_sound_fifo_dma_completion_irq() {
    TestBus bus;
    gba::GbaIo io;
    gba::GbaAudio audio;
    io.set_bus(&bus);
    io.set_audio(&audio);

    for (uint32_t i = 0; i < 8; ++i) {
        bus.write32(0x02000100u + i * 4u, 0x11111111u * (i + 1u));
    }

    // DMA2 -> FIFO B: repeat, 32-bit, special/FIFO timing, completion IRQ.
    io.write32(0xC8u, 0x02000100u);
    io.write32(0xCCu, 0x040000A4u);
    io.write16(0xD0u, 4u);
    io.write16(0xD2u, 0xF640u);

    // Timer 0 clocks both direct-sound FIFOs by default. Its first overflow
    // requests an empty FIFO refill, which is one four-word DMA burst.
    io.write16(0x100u, 0xFFFEu);
    io.write16(0x102u, 0x0080u);
    io.tick_timers(2u);

    check_eq("sound_fifo_dma_run_count_1",
             static_cast<uint32_t>(io.dma_runs(2)), 1u);
    check_eq("sound_fifo_dma_word_count_1",
             static_cast<uint32_t>(io.dma_words(2)), 4u);
    check_eq("sound_fifo_dma_irq_1",
             io.if_reg() & gba::GbaIo::IrqDma2, gba::GbaIo::IrqDma2);

    // Completion IRQ is per burst, not just the channel's initial enable.
    io.write16(gba::IoReg::IF, gba::GbaIo::IrqDma2);
    io.tick_timers(2u);
    check_eq("sound_fifo_dma_run_count_2",
             static_cast<uint32_t>(io.dma_runs(2)), 2u);
    check_eq("sound_fifo_dma_irq_2",
             io.if_reg() & gba::GbaIo::IrqDma2, gba::GbaIo::IrqDma2);
}

void test_mmio_cap_records_dma_kicked_from_write32() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.set_io(&io);
    bus.write16(0x02000000u, 0xBEEFu);

    const uint64_t t0 = gba::gba_mmio_cap_total();
    io.write32(0xB0u, 0x02000000u);  // DMA0 SAD
    io.write32(0xB4u, 0x04000204u);  // DMA0 DAD = WAITCNT (plain store)
    // One 32-bit store covering CNT_L+CNT_H: the CNT_H half kicks an
    // immediate 16-bit transfer whose destination write reenters
    // GbaIo::write16 while the split is still being decomposed.
    io.write32(0xB8u, 0x80000001u);  // CNT_L=1, CNT_H=enable+immediate
    const uint64_t t1 = gba::gba_mmio_cap_total();

    // 3 size-4 stores + the reentrant destination write. The old global
    // split flag swallowed the last one (delta was 3).
    check_eq("mmio_cap_dma_reentrancy_delta",
             static_cast<uint32_t>(t1 - t0), 4u);

    std::array<gba::MmioCapEntry, 4> entries{};
    uint64_t first = 0;
    const std::size_t n =
        gba::gba_mmio_cap_query(t0, entries.size(), entries.data(), first);
    check_eq("mmio_cap_dma_reentrancy_query", static_cast<uint32_t>(n), 4u);
    if (n == 4) {
        check_eq("mmio_cap_dma_reentrancy_sad_addr", entries[0].addr,
                 0x040000B0u);
        check_eq("mmio_cap_dma_reentrancy_sad_size", entries[0].size, 4u);
        check_eq("mmio_cap_dma_reentrancy_dad_addr", entries[1].addr,
                 0x040000B4u);
        check_eq("mmio_cap_dma_reentrancy_dad_size", entries[1].size, 4u);
        check_eq("mmio_cap_dma_reentrancy_cnt_addr", entries[2].addr,
                 0x040000B8u);
        check_eq("mmio_cap_dma_reentrancy_cnt_size", entries[2].size, 4u);
        check_eq("mmio_cap_dma_reentrancy_dest_addr", entries[3].addr,
                 0x04000204u);
        check_eq("mmio_cap_dma_reentrancy_dest_size", entries[3].size, 2u);
        check_eq("mmio_cap_dma_reentrancy_dest_value", entries[3].value,
                 0xBEEFu);
    }
    check_eq("mmio_cap_dma_reentrancy_guest",
             io.read16(gba::IoReg::WAITCNT), 0xBEEFu);
    check_eq("mmio_cap_dma_reentrancy_runs",
             static_cast<uint32_t>(io.dma_runs(0)), 1u);
}

}  // namespace

int main() {
#if defined(_WIN32)
    _putenv_s("GBARECOMP_MMIO_CAP", "1");
#else
    setenv("GBARECOMP_MMIO_CAP", "1", 1);
#endif
    test_mmio_cap_records_dma_kicked_from_write32();
    test_immediate_word_alignment();
    test_immediate_halfword_alignment();
    test_timed_word_alignment();
    test_sound_fifo_dma_completion_irq();
    if (failures) {
        std::printf("dma_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("dma_tests: OK\n");
    return 0;
}
