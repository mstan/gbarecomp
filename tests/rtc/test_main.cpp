#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)
#  include <unistd.h>
#endif

#include "gba_rtc.h"

namespace {

constexpr uint8_t kSck = 0x1;
constexpr uint8_t kSio = 0x2;
constexpr uint8_t kCs  = 0x4;

int failures = 0;

void check_eq(const char* test, const char* tag, unsigned got, unsigned expect) {
    if (got != expect) {
        std::printf("FAIL %s: %s mismatch (got 0x%X, expected 0x%X)\n",
                    test, tag, got, expect);
        ++failures;
    }
}

void check_true(const char* test, const char* tag, bool cond) {
    if (!cond) {
        std::printf("FAIL %s: %s\n", test, tag);
        ++failures;
    }
}

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void clear_env(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

void start(gba::GbaRtc& rtc) {
    rtc.gpio_write(0);
    rtc.gpio_write(kCs);
}

void stop(gba::GbaRtc& rtc) {
    rtc.gpio_write(0);
}

void write_bit(gba::GbaRtc& rtc, bool bit) {
    const uint8_t pins = static_cast<uint8_t>(kCs | (bit ? kSio : 0));
    rtc.gpio_write(pins);
    rtc.gpio_write(static_cast<uint8_t>(pins | kSck));
    rtc.gpio_write(pins);
}

bool read_bit(gba::GbaRtc& rtc) {
    rtc.gpio_write(kCs);
    rtc.gpio_write(static_cast<uint8_t>(kCs | kSck));
    const bool bit = rtc.gpio_drive(1) > 0;
    rtc.gpio_write(kCs);
    return bit;
}

void write_byte_msb(gba::GbaRtc& rtc, uint8_t value) {
    for (int bit = 7; bit >= 0; --bit) {
        write_bit(rtc, ((value >> bit) & 1u) != 0);
    }
}

void write_byte_lsb(gba::GbaRtc& rtc, uint8_t value) {
    for (int bit = 0; bit < 8; ++bit) {
        write_bit(rtc, ((value >> bit) & 1u) != 0);
    }
}

uint8_t read_byte_lsb(gba::GbaRtc& rtc) {
    uint8_t value = 0;
    for (int bit = 0; bit < 8; ++bit) {
        if (read_bit(rtc)) value = static_cast<uint8_t>(value | (1u << bit));
    }
    return value;
}

void write_reg(gba::GbaRtc& rtc, uint8_t cmd, const uint8_t* bytes, int len) {
    start(rtc);
    write_byte_msb(rtc, cmd);
    for (int i = 0; i < len; ++i) write_byte_lsb(rtc, bytes[i]);
    stop(rtc);
}

template <std::size_t N>
std::array<uint8_t, N> read_reg(gba::GbaRtc& rtc, uint8_t cmd) {
    std::array<uint8_t, N> out{};
    start(rtc);
    write_byte_msb(rtc, cmd);
    for (std::size_t i = 0; i < N; ++i) out[i] = read_byte_lsb(rtc);
    stop(rtc);
    return out;
}

gba::GbaRtc configured_rtc() {
    static constexpr uint8_t kRomWithRtc[] = {
        'x', 'x', 'S', 'I', 'I', 'R', 'T', 'C', '_', 'V', 'x', 'x',
    };
    gba::GbaRtc rtc;
    rtc.configure(kRomWithRtc, sizeof(kRomWithRtc));
    return rtc;
}

void test_boot_seed_and_battery_status() {
    const char* T = "boot_seed";
    set_env("RECOMP_RTC_EPOCH", "946684800");  // 2000-01-01 00:00:00 Sat
    clear_env("RECOMP_RTC_OFF");

    gba::GbaRtc rtc = configured_rtc();
    check_true(T, "rtc signature activates device", rtc.active());

    auto control = read_reg<1>(rtc, 0x63);
    check_eq(T, "power-lost flag starts clear", control[0] & 0x80u, 0);

    const uint8_t h24 = 0x40;
    write_reg(rtc, 0x62, &h24, 1);
    auto dt = read_reg<7>(rtc, 0x65);
    const uint8_t expect[] = {0x00, 0x01, 0x01, 0x06, 0x00, 0x00, 0x00};
    for (int i = 0; i < 7; ++i) {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "datetime[%d]", i);
        check_eq(T, tag, dt[static_cast<std::size_t>(i)], expect[i]);
    }
}

void test_guest_set_clock_is_local() {
    const char* T = "set_clock";
    set_env("RECOMP_RTC_EPOCH", "946684800");
    clear_env("RECOMP_RTC_OFF");

    gba::GbaRtc rtc = configured_rtc();
    const uint8_t h24 = 0x40;
    write_reg(rtc, 0x62, &h24, 1);

    const uint8_t target[] = {0x24, 0x02, 0x29, 0x04, 0x23, 0x58, 0x59};
    write_reg(rtc, 0x64, target, 7);
    auto dt = read_reg<7>(rtc, 0x65);
    for (int i = 0; i < 7; ++i) {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "datetime[%d]", i);
        check_eq(T, tag, dt[static_cast<std::size_t>(i)], target[i]);
    }
}

void test_reset_clears_power_lost_without_resyncing_host() {
    const char* T = "reset";
    set_env("RECOMP_RTC_EPOCH", "946684800");
    clear_env("RECOMP_RTC_OFF");

    gba::GbaRtc rtc = configured_rtc();
    const uint8_t bad_control = 0xC0;
    write_reg(rtc, 0x62, &bad_control, 1);
    auto before = read_reg<1>(rtc, 0x63);
    check_eq(T, "test setup sets power-lost flag", before[0] & 0x80u, 0x80);

    start(rtc);
    write_byte_msb(rtc, 0x60);
    stop(rtc);

    auto after = read_reg<1>(rtc, 0x63);
    check_eq(T, "reset leaves battery-present state", after[0] & 0x80u, 0);
}

}  // namespace

int main() {
    test_boot_seed_and_battery_status();
    test_guest_set_clock_is_local();
    test_reset_clears_power_lost_without_resyncing_host();

    if (failures) {
        std::printf("rtc_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("rtc_tests: all checks passed\n");
    return 0;
}
