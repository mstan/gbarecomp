// gba_rtc.h — cartridge GPIO port + S-3511A Real-Time Clock.
//
// Device emulation (NOT HLE): the guest's own recompiled driver bit-bangs
// the 3 serial pins on the GPIO data port at 0x080000C4..0x080000C9; we
// model the chip it talks to, exactly as we model the PPU or a timer. No
// guest code is skipped. Clock cartridges (Pokémon Ruby/Sapphire/Emerald,
// etc.) are detected by the Seiko SDK library signature; carts without the
// chip are untouched (their 0xC4 region reads ordinary ROM).
//
// On boot, the RTC is seeded once from the host's local civil time. After that
// reads come from the emulated chip's own monotonic timeline, so host clock
// changes do not resync the game. A game that *sets* the clock is honored for
// the session via a seconds offset. RECOMP_RTC_OFF disables; RECOMP_RTC_EPOCH
// pins a deterministic time for reproducible differential runs.
//
// ── Attribution ───────────────────────────────────────────────────
// Ported from JRickey/gba-recomp (https://github.com/JRickey/gba-recomp),
// crates/gba-core/src/{rtc,hostclock}.rs, © Jrickey, MIT OR Apache-2.0,
// used with permission. C++ port is ours. See THIRD_PARTY_ATTRIBUTION.md.

#pragma once

#include <cstddef>
#include <cstdint>

#include "gba_gpio.h"

namespace gbarecomp::debug {
class SnapshotWriter;
class SnapshotReader;
}  // namespace gbarecomp::debug

namespace gba {

class GbaRtc final : public GpioDevice {
public:
    GbaRtc();

    // Scan a ROM image for the Seiko RTC library signature and enable the
    // clock if present (honors RECOMP_RTC_OFF). Call once after the ROM is
    // wired to the bus.
    void configure(const uint8_t* rom, std::size_t len);

    bool active() const { return active_; }

    // ── GpioDevice ───────────────────────────────────────────────────
    // The port owns the data/direction/control registers now; the clock only
    // models the chip on the wires. Pins: 0 = SCK, 1 = SIO, 2 = CS, 3 unused.
    bool gpio_active() const override { return active_; }
    void gpio_write(uint8_t pins) override;
    int  gpio_drive(uint8_t bit) const override;

    void serialize(gbarecomp::debug::SnapshotWriter& w) const;
    void deserialize(gbarecomp::debug::SnapshotReader& r);

private:
    enum class Phase { Idle, Command, Read, Write };

    // Broken-down local civil time (dow 0=Sun..6=Sat).
    struct Civil {
        int     year;
        uint8_t month, day, dow, hour, min, sec;
    };

    void    clock_bit(bool sio_in);
    uint8_t bit_pos() const;
    void    advance_byte(bool committing);
    void    decode_command();
    void    load_datetime();
    void    load_time();
    void    commit_write();
    void    reset_clock();
    void    seed_boot_time();
    int64_t linear_seconds(const Civil& c) const;
    int64_t current_seconds() const;
    Civil   civil_from_seconds(int64_t lin) const;
    Civil   host_now() const;
    Civil   now() const;
    void    set_offset_from(const Civil& target);

    bool active_ = false;

    // Serial line state.
    bool sck_ = false;
    bool cs_ = false;
    bool sio_out_ = false;

    // Transaction state machine.
    Phase   phase_ = Phase::Idle;
    uint8_t cmd_acc_ = 0;
    uint8_t nbits_ = 0;
    std::size_t byte_idx_ = 0;
    std::size_t buflen_ = 0;
    uint8_t buffer_[7] = {};
    uint8_t reg_ = 0;
    bool    lsb_first_ = false;

    uint8_t control_ = 0;   // bit6 = 24h mode, bit7 = power-lost
    int64_t offset_ = 0;    // seconds added to boot timeline when game sets clock

    bool    boot_seeded_ = false;
    int64_t boot_seconds_ = 0;
    int64_t boot_monotonic_seconds_ = 0;

    bool    have_fixed_ = false;  // RECOMP_RTC_EPOCH override
    int64_t fixed_ = 0;
    bool    trace_ = false;
};

}  // namespace gba
