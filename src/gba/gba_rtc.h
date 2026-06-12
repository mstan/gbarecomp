// gba_rtc.h — cartridge GPIO port + S-3511A Real-Time Clock.
//
// Device emulation (NOT HLE): the guest's own recompiled driver bit-bangs
// the 3 serial pins on the GPIO data port at 0x080000C4..0x080000C9; we
// model the chip it talks to, exactly as we model the PPU or a timer. No
// guest code is skipped. Clock cartridges (Pokémon Ruby/Sapphire/Emerald,
// etc.) are detected by the Seiko SDK library signature; carts without the
// chip are untouched (their 0xC4 region reads ordinary ROM).
//
// Reads are answered from the HOST clock (no battery). A game that *sets*
// the clock is honored for the session via a seconds offset, so the in-game
// clock advances with real time. RECOMP_RTC_OFF disables; RECOMP_RTC_EPOCH
// pins a deterministic time for reproducible differential runs.
//
// ── Attribution ───────────────────────────────────────────────────
// Ported from JRickey/gba-recomp (https://github.com/JRickey/gba-recomp),
// crates/gba-core/src/{rtc,hostclock}.rs, © Jrickey, MIT OR Apache-2.0,
// used with permission. C++ port is ours. See THIRD_PARTY_ATTRIBUTION.md.

#pragma once

#include <cstddef>
#include <cstdint>

namespace gba {

class GbaRtc {
public:
    GbaRtc();

    // Scan a ROM image for the Seiko RTC library signature and enable the
    // clock if present (honors RECOMP_RTC_OFF). Call once after the ROM is
    // wired to the bus.
    void configure(const uint8_t* rom, std::size_t len);

    bool active() const { return active_; }
    bool read_enabled() const { return read_enable_; }

    // GPIO register access. `off` is the ROM-relative offset (0xC4..0xC9);
    // the bus routes those addresses here while the clock is active.
    uint8_t read(uint32_t off) const;
    void    write(uint32_t off, uint8_t value);

private:
    enum class Phase { Idle, Command, Read, Write };

    // Broken-down local civil time (dow 0=Sun..6=Sat).
    struct Civil {
        int     year;
        uint8_t month, day, dow, hour, min, sec;
    };

    uint8_t read_data() const;
    void    write_data(uint8_t value);
    void    clock_bit(bool sio_in);
    uint8_t bit_pos() const;
    void    advance_byte(bool committing);
    void    decode_command();
    void    load_datetime();
    void    load_time();
    void    commit_write();
    void    reset_clock();
    Civil   base_now() const;
    Civil   now() const;
    void    set_offset_from(const Civil& target);

    bool active_ = false;

    // GPIO port registers (low nibble significant).
    uint8_t data_ = 0;
    uint8_t direction_ = 0;
    bool    read_enable_ = false;

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
    int64_t offset_ = 0;    // seconds added to host time when game sets clock

    bool    have_fixed_ = false;  // RECOMP_RTC_EPOCH override
    int64_t fixed_ = 0;
    bool    trace_ = false;
};

}  // namespace gba
