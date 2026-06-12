// gba_m4a.h — MP2K ("m4a") sound-driver detection + live SoundInfo dump.
//
// OBSERVABILITY ONLY. Nothing here is on a runtime exec path: the
// recompiled guest driver still runs unchanged. This module (a) scans
// a ROM image for the SDK MusicPlayer2000 driver and (b) reads the
// driver's live SoundInfo / SoundChannel state out of guest memory so
// the TCP debug server can surface it. It is a diagnostic lens — see
// PRINCIPLES.md: it does not HLE, substitute, or alter execution.
//
// Primary use: MC-HP-002. The freeze spins in the M4A mixer walking a
// corrupt wave pointer (R1 in the 0x0046xxxx open-bus range). Dumping
// the channel array shows which voice's wav/data/count field went bad
// and when, straight out of the always-on state.
//
// ── Attribution ───────────────────────────────────────────────────
// The driver-detection signature shape and the SoundInfo /
// SoundChannel / WaveData struct offsets are ported from
// JRickey/gba-recomp (https://github.com/JRickey/gba-recomp),
// crates/gba-core/src/mp2k.rs, © Jrickey, dual-licensed MIT OR
// Apache-2.0, used with permission. Facts (offsets, signatures) +
// detection logic ported; the C++ and the live-dump path are ours.
// See THIRD_PARTY_ATTRIBUTION.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gba {

class GbaBus;

// ── MP2K guest-struct map (SDK MusicPlayer2000) ────────────────────
// SoundInfo is reached through a pointer the SDK parks in IWRAM.
inline constexpr uint32_t kSoundInfoPtr   = 0x03007FF0u; // *SoundInfo
inline constexpr uint32_t kMp2kMagicBase  = 0x68736D53u; // 'Smsh'
inline constexpr uint32_t kMp2kMagicLive  = 0x68736D54u; // 'Smsh'+1 (tick in flight)
inline constexpr uint32_t kSoundChansOff  = 0x50u;       // first SoundChannel
inline constexpr uint32_t kSoundChanStride= 0x40u;       // stride
inline constexpr int      kMp2kMaxChans   = 12;

// A detected stock MP2K driver in a ROM image.
struct Mp2kSig {
    std::size_t sound_main_off = 0;   // ROM offset of SoundMain's first insn
    uint32_t    sound_main_ram = 0;   // SoundMainRAM entry (bit0 = thumb)
};

// Scan a ROM image for the MP2K driver. Primary signal is SoundMain's
// literal-pool shape (SOUND_INFO_PTR, ID 'Smsh', SoundMainRAM|thumb,
// REG_VCOUNT 0x04000006, o_pcmBuffer 0x350); CRC-of-SoundMain fallback
// for revisions whose first 48 bytes differ. Returns every distinct
// match (some images link more than one driver copy).
std::vector<Mp2kSig> mp2k_detect(const uint8_t* rom, std::size_t len);

// Read the driver's live SoundInfo + channel array out of guest memory
// and append a JSON object to `out`. Self-contained: finds SoundInfo
// via the IWRAM pointer, reports liveness from the ident magic, and
// flags any channel whose wav/data/count field points outside a sane
// region (the corrupt-pointer signature). Never mutates guest state.
void mp2k_dump_live(GbaBus& bus, std::string& out);

}  // namespace gba
