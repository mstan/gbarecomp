// gba_m4a.cpp — see gba_m4a.h. Observability only; no exec path.
//
// Detection logic + struct offsets ported from JRickey/gba-recomp
// (crates/gba-core/src/mp2k.rs), © Jrickey, MIT OR Apache-2.0, used
// with permission. See THIRD_PARTY_ATTRIBUTION.md.

#include "gba_m4a.h"

#include <cstdio>

#include "gba_bus.h"

namespace gba {
namespace {

// SoundMain's first 48 bytes are linker-identical for the stock driver
// revision the CRC fallback targets; its literal pool holds the
// SoundMainRAM pointer at +0x74 from the matched window.
constexpr uint32_t kSoundMainCrc       = 0x27EA7FCFu;
constexpr std::size_t kSigLen          = 48;
constexpr std::size_t kSoundMainRamOff = 0x74;

uint32_t rd32(const uint8_t* p, std::size_t i) {
    return static_cast<uint32_t>(p[i]) | (static_cast<uint32_t>(p[i + 1]) << 8) |
           (static_cast<uint32_t>(p[i + 2]) << 16) |
           (static_cast<uint32_t>(p[i + 3]) << 24);
}

// IEEE-802.3 reflected CRC32 (init/xorout 0xFFFFFFFF), table-driven —
// the variant kSoundMainCrc was computed with.
uint32_t crc32_window(const uint8_t* p, std::size_t len) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                uint32_t mask = -(c & 1u);
                c = (c >> 1) ^ (0xEDB88320u & mask);
            }
            table[i] = c;
        }
        built = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFFu];
    return ~crc;
}

// A SoundMainRAM / wave / data pointer should land in EWRAM, IWRAM, or
// cartridge ROM. Anything else (BIOS/open-bus/unmapped) is corruption.
bool ptr_region_ok(uint32_t addr) {
    uint32_t region = addr >> 24;
    return region == 0x02 || region == 0x03 ||
           (region >= 0x08 && region <= 0x0D);
}

}  // namespace

std::vector<Mp2kSig> mp2k_detect(const uint8_t* rom, std::size_t len) {
    std::vector<Mp2kSig> found;
    if (!rom || len < 20) return found;

    // Primary: SoundMain literal-pool shape, scanned at word stride.
    constexpr uint32_t kNeedleLo = 0x03007FF0u;  // SOUND_INFO_PTR
    constexpr uint32_t kNeedleHi = 0x68736D53u;  // ID 'Smsh'
    for (std::size_t off = 0; off + 20 <= len; off += 4) {
        if (rd32(rom, off) != kNeedleLo || rd32(rom, off + 4) != kNeedleHi)
            continue;
        uint32_t ptr = rd32(rom, off + 8);
        uint32_t reg = ptr >> 24;
        bool plausible = (reg == 2 || reg == 3) && ((ptr & 1) != 0 || (ptr & 3) == 0);
        // 0x04000006 = REG_VCOUNT; 0x350 = o_pcmBuffer — the 0x350 gate
        // rejects driver variants that resized the channel array (their
        // struct offsets would not match ours).
        if (!plausible || rd32(rom, off + 12) != 0x04000006u ||
            rd32(rom, off + 16) != 0x350u)
            continue;
        bool dup = false;
        for (const auto& f : found)
            if (f.sound_main_ram == ptr) dup = true;
        if (!dup && found.size() < 4)
            found.push_back(Mp2kSig{off, ptr});
    }
    if (!found.empty()) return found;

    // Fallback: 48-byte CRC of SoundMain + the +0x74 pool harvest.
    if (len < kSigLen + 4) return found;
    for (std::size_t off = 0; off + kSigLen + 4 <= len; off += 2) {
        if (crc32_window(rom + off, kSigLen) != kSoundMainCrc) continue;
        uint32_t ptr = rd32(rom, off + kSoundMainRamOff);
        uint32_t reg = ptr >> 24;
        if (reg == 0x03 || reg == 0x02) found.push_back(Mp2kSig{off, ptr});
        break;  // one revision; first match wins
    }
    return found;
}

void mp2k_dump_live(GbaBus& bus, std::string& out) {
    auto appendf = [&out](const char* fmt, auto... args) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        out += buf;
    };

    uint32_t si = bus.read32(kSoundInfoPtr);
    uint32_t si_region = si >> 24;
    if (si_region != 0x02 && si_region != 0x03) {
        appendf("{\"ok\":true,\"live\":false,\"reason\":\"SoundInfo ptr "
                "0x%08x out of RAM\",\"sound_info\":%u}",
                si, static_cast<unsigned>(si));
        return;
    }

    uint32_t ident   = bus.read32(si);
    bool     live    = ident == kMp2kMagicLive;
    uint8_t  reverb  = static_cast<uint8_t>(bus.read8(si + 0x05)) & 0x7Fu;
    uint8_t  maxch_r = static_cast<uint8_t>(bus.read8(si + 0x06));
    int      maxch   = maxch_r < 1 ? 1 : (maxch_r > kMp2kMaxChans ? kMp2kMaxChans : maxch_r);
    uint8_t  master  = static_cast<uint8_t>(bus.read8(si + 0x07)) & 0x0Fu;
    uint32_t spv     = bus.read32(si + 0x10);
    uint32_t pcmfreq = bus.read32(si + 0x14);

    appendf("{\"ok\":true,\"sound_info\":%u,\"ident\":%u,\"live\":%s,"
            "\"master_vol\":%u,\"reverb\":%u,\"max_chans\":%u,"
            "\"pcm_freq\":%u,\"spv\":%u,\"channels\":[",
            static_cast<unsigned>(si), static_cast<unsigned>(ident),
            live ? "true" : "false", master, reverb,
            static_cast<unsigned>(maxch), static_cast<unsigned>(pcmfreq),
            static_cast<unsigned>(spv));

    bool any_corrupt = false;
    for (int ch = 0; ch < kMp2kMaxChans; ++ch) {
        uint32_t base = si + kSoundChansOff + static_cast<uint32_t>(ch) * kSoundChanStride;
        uint8_t  status = static_cast<uint8_t>(bus.read8(base + 0x00));
        uint8_t  type   = static_cast<uint8_t>(bus.read8(base + 0x01));
        bool     active = (status & 0xC7u) != 0;
        uint32_t count  = bus.read32(base + 0x18);
        uint32_t freq   = bus.read32(base + 0x20);
        uint32_t wav    = bus.read32(base + 0x24);

        // Wave header (16B at wav): type, flags(loop bits), -, loopStart, size.
        uint32_t wsize = 0, wloop = 0, wflags = 0;
        bool wav_ok = ptr_region_ok(wav);
        bool wav_hdr_ok = false;
        if (wav_ok) {
            wflags = bus.read16(wav + 0x02);
            wloop  = bus.read32(wav + 0x08);
            wsize  = bus.read32(wav + 0x0C);
            wav_hdr_ok = wsize != 0 && wsize <= 0x01000000u && wloop <= wsize;
        }
        // The MC-HP-002 corruption signature: an active PCM voice whose
        // wave pointer (or count offset, used as a data index) is not in
        // a region samples can live in.
        bool corrupt = active && (type & 0x07u) == 0 &&
                       (!wav_ok || !wav_hdr_ok || !ptr_region_ok(count + wav));
        if (corrupt) any_corrupt = true;

        appendf("%s{\"ch\":%d,\"active\":%s,\"status\":%u,\"type\":%u,"
                "\"vol_r\":%u,\"vol_l\":%u,\"env\":%u,\"count\":%u,"
                "\"freq\":%u,\"wav\":%u,\"wav_region_ok\":%s,"
                "\"wav_hdr_ok\":%s,\"wav_size\":%u,\"wav_loop\":%u,"
                "\"loop\":%s,\"corrupt\":%s}",
                ch == 0 ? "" : ",", ch, active ? "true" : "false",
                status, type,
                static_cast<unsigned>(bus.read8(base + 0x02)),
                static_cast<unsigned>(bus.read8(base + 0x03)),
                static_cast<unsigned>(bus.read8(base + 0x09)),
                static_cast<unsigned>(count), static_cast<unsigned>(freq),
                static_cast<unsigned>(wav), wav_ok ? "true" : "false",
                wav_hdr_ok ? "true" : "false",
                static_cast<unsigned>(wsize), static_cast<unsigned>(wloop),
                (wflags & 0xC000u) ? "true" : "false",
                corrupt ? "true" : "false");
    }
    appendf("],\"any_corrupt\":%s}", any_corrupt ? "true" : "false");
}

}  // namespace gba
