// gba_rtc.cpp — see gba_rtc.h.
//
// Ported from JRickey/gba-recomp (crates/gba-core/src/{rtc,hostclock}.rs),
// © Jrickey, MIT OR Apache-2.0, used with permission. C++ port is ours.
// See THIRD_PARTY_ATTRIBUTION.md.

#include "gba_rtc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <ctime>
#endif

namespace gba {
namespace {

constexpr char kSignature[] = "SIIRTC_V";  // Seiko SDK library tag (8 bytes)
constexpr std::size_t kSigLen = 8;

uint8_t bcd(uint8_t v)   { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
uint8_t unbcd(uint8_t b) { return static_cast<uint8_t>((b >> 4) * 10 + (b & 0x0F)); }

uint8_t reverse_bits(uint8_t b) {
    b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

// Hour (0-23) → RTC hour byte: BCD low bits + PM flag (bit7). 12-hour mode
// uses 1-12 in the low bits.
uint8_t encode_hour(uint8_t hour, bool h24) {
    uint8_t pm = hour >= 12 ? 1 : 0;
    uint8_t h;
    if (h24) {
        h = hour;
    } else {
        h = hour % 12;
        if (h == 0) h = 12;
    }
    return static_cast<uint8_t>(bcd(h) | (pm << 7));
}

uint8_t decode_hour(uint8_t byte, bool h24) {
    bool pm = (byte & 0x80) != 0;
    uint8_t h = unbcd(byte & 0x3F);
    if (h24) return h > 23 ? 23 : h;
    h = h % 12;
    return pm ? static_cast<uint8_t>(h + 12) : h;
}

// ── Civil <-> linear-seconds (Howard Hinnant, public domain) ───────
int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y = (m <= 2) ? y - 1 : y;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t z, int64_t& y, int64_t& m, int64_t& d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y += 1;
}

int64_t mod_euclid(int64_t a, int64_t b) {
    int64_t r = a % b;
    return r < 0 ? r + b : r;
}
int64_t div_euclid(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

}  // namespace

GbaRtc::GbaRtc() {
    if (const char* e = std::getenv("RECOMP_RTC_EPOCH")) {
        char* end = nullptr;
        long long v = std::strtoll(e, &end, 10);
        if (end && end != e) { have_fixed_ = true; fixed_ = v; }
    }
    trace_ = std::getenv("RECOMP_TRACE_RTC") != nullptr;
}

void GbaRtc::configure(const uint8_t* rom, std::size_t len) {
    active_ = false;
    if (std::getenv("RECOMP_RTC_OFF") != nullptr) return;
    if (!rom || len < kSigLen) return;
    for (std::size_t i = 0; i + kSigLen <= len; ++i) {
        if (std::memcmp(rom + i, kSignature, kSigLen) == 0) { active_ = true; break; }
    }
}

uint8_t GbaRtc::read(uint32_t off) const {
    if (!read_enable_) return 0;
    switch (off) {
        case 0xC4: return read_data();
        case 0xC6: return direction_;
        case 0xC8: return 1;
        default:   return 0;  // high bytes of each halfword register read 0
    }
}

void GbaRtc::write(uint32_t off, uint8_t value) {
    switch (off) {
        case 0xC4: write_data(value); break;
        case 0xC6: direction_ = value & 0x0F; break;
        case 0xC8: read_enable_ = (value & 1) != 0; break;
        default:   break;
    }
}

uint8_t GbaRtc::read_data() const {
    uint8_t v = 0;
    for (uint8_t bit = 0; bit < 4; ++bit) {
        uint8_t level;
        if (direction_ & (1 << bit)) {
            level = (data_ >> bit) & 1;
        } else if (bit == 1) {
            level = sio_out_ ? 1 : 0;
        } else {
            level = 0;
        }
        v |= static_cast<uint8_t>(level << bit);
    }
    return v;
}

void GbaRtc::write_data(uint8_t value) {
    data_ = value & 0x0F;
    bool new_sck = (data_ & 0b001) != 0;
    bool new_cs  = (data_ & 0b100) != 0;
    bool sio_in  = (data_ & 0b010) != 0;

    if (!cs_ && new_cs) {
        phase_ = Phase::Command;
        cmd_acc_ = 0;
        nbits_ = 0;
    } else if (cs_ && !new_cs) {
        phase_ = Phase::Idle;
    }
    if (new_cs && !sck_ && new_sck) clock_bit(sio_in);
    sck_ = new_sck;
    cs_  = new_cs;
}

void GbaRtc::clock_bit(bool sio_in) {
    switch (phase_) {
        case Phase::Command:
            cmd_acc_ = static_cast<uint8_t>((cmd_acc_ << 1) | (sio_in ? 1 : 0));
            ++nbits_;
            if (nbits_ == 8) decode_command();
            break;
        case Phase::Write:
            if (byte_idx_ < buflen_) {
                uint8_t pos = bit_pos();
                if (sio_in) buffer_[byte_idx_] |= static_cast<uint8_t>(1 << pos);
                else        buffer_[byte_idx_] &= static_cast<uint8_t>(~(1 << pos));
                advance_byte(true);
            }
            break;
        case Phase::Read:
            if (byte_idx_ < buflen_) {
                uint8_t pos = bit_pos();
                sio_out_ = ((buffer_[byte_idx_] >> pos) & 1) != 0;
                advance_byte(false);
            }
            break;
        case Phase::Idle:
            break;
    }
}

uint8_t GbaRtc::bit_pos() const {
    return lsb_first_ ? nbits_ : static_cast<uint8_t>(7 - nbits_);
}

void GbaRtc::advance_byte(bool committing) {
    ++nbits_;
    if (nbits_ == 8) {
        nbits_ = 0;
        ++byte_idx_;
        if (committing && byte_idx_ == buflen_) commit_write();
    }
}

void GbaRtc::decode_command() {
    uint8_t cmd;
    bool lsb;
    if ((cmd_acc_ >> 4) == 0b0110) {
        cmd = cmd_acc_;
        lsb = false;
    } else {
        cmd = reverse_bits(cmd_acc_);
        lsb = true;
    }
    lsb_first_ = lsb;
    reg_ = (cmd >> 1) & 0x07;
    bool read = (cmd & 1) != 0;
    nbits_ = 0;
    byte_idx_ = 0;
    std::memset(buffer_, 0, sizeof(buffer_));
    switch (reg_) {
        case 1:  buflen_ = 1; break;
        case 2:  buflen_ = 7; break;
        case 3:  buflen_ = 3; break;
        default: buflen_ = 0; break;
    }
    if (reg_ == 0) reset_clock();
    if (read) {
        switch (reg_) {
            case 1: buffer_[0] = control_; break;
            case 2: load_datetime(); break;
            case 3: load_time(); break;
            default: break;
        }
        phase_ = Phase::Read;
    } else {
        phase_ = Phase::Write;
    }
    if (trace_) {
        std::fprintf(stderr, "rtc: cmd reg=%u %s len=%zu %s\n", reg_,
                     read ? "read" : "write", buflen_, lsb ? "lsb" : "msb");
    }
}

void GbaRtc::load_datetime() {
    Civil c = now();
    bool h24 = (control_ & 0x40) != 0;
    buffer_[0] = bcd(static_cast<uint8_t>(mod_euclid(c.year, 100)));
    buffer_[1] = bcd(c.month);
    buffer_[2] = bcd(c.day);
    buffer_[3] = c.dow & 0x07;
    buffer_[4] = encode_hour(c.hour, h24);
    buffer_[5] = bcd(c.min);
    buffer_[6] = bcd(c.sec);
}

void GbaRtc::load_time() {
    Civil c = now();
    bool h24 = (control_ & 0x40) != 0;
    buffer_[0] = encode_hour(c.hour, h24);
    buffer_[1] = bcd(c.min);
    buffer_[2] = bcd(c.sec);
}

void GbaRtc::commit_write() {
    switch (reg_) {
        case 1:
            control_ = buffer_[0];
            break;
        case 2: {
            bool h24 = (control_ & 0x40) != 0;
            Civil set;
            set.year  = 2000 + unbcd(buffer_[0]);
            uint8_t mo = unbcd(buffer_[1]); set.month = mo < 1 ? 1 : (mo > 12 ? 12 : mo);
            uint8_t da = unbcd(buffer_[2]); set.day   = da < 1 ? 1 : (da > 31 ? 31 : da);
            set.dow   = buffer_[3] & 0x07;
            set.hour  = decode_hour(buffer_[4], h24);
            uint8_t mi = unbcd(buffer_[5]); set.min = mi > 59 ? 59 : mi;
            uint8_t se = unbcd(buffer_[6]); set.sec = se > 59 ? 59 : se;
            set_offset_from(set);
            break;
        }
        case 3: {
            bool h24 = (control_ & 0x40) != 0;
            Civil set = now();
            set.hour = decode_hour(buffer_[0], h24);
            uint8_t mi = unbcd(buffer_[1]); set.min = mi > 59 ? 59 : mi;
            uint8_t se = unbcd(buffer_[2]); set.sec = se > 59 ? 59 : se;
            set_offset_from(set);
            break;
        }
        default:
            break;
    }
}

GbaRtc::Civil GbaRtc::base_now() const {
    if (have_fixed_) {
        int64_t s = fixed_;
        int64_t days = div_euclid(s, 86400);
        int64_t rem  = mod_euclid(s, 86400);
        int64_t y, m, d;
        civil_from_days(days, y, m, d);
        Civil c;
        c.year = static_cast<int>(y);
        c.month = static_cast<uint8_t>(m);
        c.day = static_cast<uint8_t>(d);
        c.dow = static_cast<uint8_t>(mod_euclid(days + 4, 7));
        c.hour = static_cast<uint8_t>(rem / 3600);
        c.min = static_cast<uint8_t>(rem % 3600 / 60);
        c.sec = static_cast<uint8_t>(rem % 60);
        return c;
    }
    Civil c{2000, 1, 1, 6, 0, 0, 0};  // fallback: 2000-01-01 Sat
#if defined(_WIN32)
    SYSTEMTIME st;
    GetLocalTime(&st);
    c.year = st.wYear;
    c.month = static_cast<uint8_t>(st.wMonth);
    c.day = static_cast<uint8_t>(st.wDay);
    c.dow = static_cast<uint8_t>(st.wDayOfWeek);
    c.hour = static_cast<uint8_t>(st.wHour);
    c.min = static_cast<uint8_t>(st.wMinute);
    c.sec = static_cast<uint8_t>(st.wSecond);
#else
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    if (localtime_r(&t, &tm)) {
        c.year = tm.tm_year + 1900;
        c.month = static_cast<uint8_t>(tm.tm_mon + 1);
        c.day = static_cast<uint8_t>(tm.tm_mday);
        c.dow = static_cast<uint8_t>(tm.tm_wday);
        c.hour = static_cast<uint8_t>(tm.tm_hour);
        c.min = static_cast<uint8_t>(tm.tm_min);
        c.sec = static_cast<uint8_t>(tm.tm_sec);
    }
#endif
    return c;
}

GbaRtc::Civil GbaRtc::now() const {
    Civil base = base_now();
    if (offset_ == 0) return base;
    int64_t lin = days_from_civil(base.year, base.month, base.day) * 86400 +
                  base.hour * 3600 + base.min * 60 + base.sec + offset_;
    int64_t days = div_euclid(lin, 86400);
    int64_t rem  = mod_euclid(lin, 86400);
    int64_t y, m, d;
    civil_from_days(days, y, m, d);
    Civil c;
    c.year = static_cast<int>(y);
    c.month = static_cast<uint8_t>(m);
    c.day = static_cast<uint8_t>(d);
    c.dow = static_cast<uint8_t>(mod_euclid(days + 4, 7));
    c.hour = static_cast<uint8_t>(rem / 3600);
    c.min = static_cast<uint8_t>(rem % 3600 / 60);
    c.sec = static_cast<uint8_t>(rem % 60);
    return c;
}

void GbaRtc::set_offset_from(const Civil& target) {
    Civil host = base_now();
    int64_t tlin = days_from_civil(target.year, target.month, target.day) * 86400 +
                   target.hour * 3600 + target.min * 60 + target.sec;
    int64_t hlin = days_from_civil(host.year, host.month, host.day) * 86400 +
                   host.hour * 3600 + host.min * 60 + host.sec;
    offset_ = tlin - hlin;
    if (trace_) {
        std::fprintf(stderr, "rtc: clock set to %04d-%02u-%02u %02u:%02u:%02u (offset %lld s)\n",
                     target.year, target.month, target.day, target.hour, target.min,
                     target.sec, static_cast<long long>(offset_));
    }
}

void GbaRtc::reset_clock() {
    control_ = 0;
    offset_ = 0;
}

}  // namespace gba
