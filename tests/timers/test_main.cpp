#include <cstdint>
#include <cstdio>

#include "gba_io.h"

namespace {

int failures = 0;

void check_eq(const char* test, uint32_t got, uint32_t expected) {
    if (got != expected) {
        std::printf("FAIL %s: got 0x%08X, expected 0x%08X\n",
                    test, got, expected);
        ++failures;
    }
}

void configure_timer(gba::GbaIo& io, int timer, uint16_t reload,
                     uint16_t control) {
    const uint32_t off = 0x100u + static_cast<uint32_t>(timer) * 4u;
    io.write16(off, reload);
    io.write16(off + 2u, control);
}

void test_base_timer_overflow_irq() {
    gba::GbaIo io;
    configure_timer(io, 0, 0xFFFEu, 0x00C0u);

    io.tick_timers(2u);

    check_eq("base counter reload", io.read16(0x100u), 0xFFFEu);
    check_eq("base overflow irq", io.if_reg() & gba::GbaIo::IrqTimer0,
             gba::GbaIo::IrqTimer0);
}

void test_timer1_count_up_and_irq() {
    gba::GbaIo io;
    configure_timer(io, 0, 0xFFFEu, 0x0080u);
    configure_timer(io, 1, 0xFFFEu, 0x00C4u);

    io.tick_timers(2u);
    check_eq("cascade first increment", io.read16(0x104u), 0xFFFFu);
    check_eq("cascade no early irq", io.if_reg() & gba::GbaIo::IrqTimer1, 0u);

    io.tick_timers(2u);
    check_eq("cascade counter reload", io.read16(0x104u), 0xFFFEu);
    check_eq("cascade overflow irq", io.if_reg() & gba::GbaIo::IrqTimer1,
             gba::GbaIo::IrqTimer1);
}

void test_multiple_overflows_propagate() {
    gba::GbaIo io;
    configure_timer(io, 0, 0xFFFFu, 0x0080u);
    configure_timer(io, 1, 0xFFFEu, 0x00C4u);

    io.tick_timers(3u);

    check_eq("multi-overflow upstream", io.read16(0x100u), 0xFFFFu);
    check_eq("multi-overflow cascade remainder", io.read16(0x104u), 0xFFFFu);
    check_eq("multi-overflow cascade irq",
             io.if_reg() & gba::GbaIo::IrqTimer1,
             gba::GbaIo::IrqTimer1);
}

void test_three_timer_count_up_chain() {
    gba::GbaIo io;
    configure_timer(io, 0, 0xFFFFu, 0x0080u);
    configure_timer(io, 1, 0xFFFFu, 0x0084u);
    configure_timer(io, 2, 0xFFFEu, 0x00C4u);

    io.tick_timers(2u);

    check_eq("chain timer1 reload", io.read16(0x104u), 0xFFFFu);
    check_eq("chain timer2 reload", io.read16(0x108u), 0xFFFEu);
    check_eq("chain timer2 irq", io.if_reg() & gba::GbaIo::IrqTimer2,
             gba::GbaIo::IrqTimer2);
}

void test_timer0_ignores_count_up_bit() {
    gba::GbaIo io;
    configure_timer(io, 0, 0xFFFFu, 0x00C4u);

    check_eq("timer0 count-up deadline", io.cycles_until_next_timer_event(), 1u);
    io.tick_timers(1u);
    check_eq("timer0 count-up ignored counter", io.read16(0x100u), 0xFFFFu);
    check_eq("timer0 count-up ignored irq",
             io.if_reg() & gba::GbaIo::IrqTimer0,
             gba::GbaIo::IrqTimer0);
}

}  // namespace

int main() {
    test_base_timer_overflow_irq();
    test_timer1_count_up_and_irq();
    test_multiple_overflows_propagate();
    test_three_timer_count_up_chain();
    test_timer0_ignores_count_up_bit();
    if (failures != 0) {
        std::printf("timer_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("timer_tests: OK\n");
    return 0;
}
