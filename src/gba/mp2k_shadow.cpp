// mp2k_shadow.cpp — see mp2k_shadow.h.
//
// Ported from JRickey/gba-recomp (crates/gba-core/src/mp2k.rs), © Jrickey,
// MIT OR Apache-2.0, used with permission. See THIRD_PARTY_ATTRIBUTION.md.

#include "mp2k_shadow.h"

#include <algorithm>
#include <cmath>

namespace gba {
namespace {

// SDK gDeltaEncodingTable.
constexpr int8_t kDpcmLut[16] = {0, 1, 4, 9, 16, 25, 36, 49,
                                 -64, -49, -36, -25, -16, -9, -4, -1};

uint32_t load_u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

const uint8_t* MemView::slice(uint32_t addr, std::size_t len) const {
    const uint8_t* region = nullptr;
    std::size_t off = 0, rlen = 0;
    switch (addr >> 24) {
        case 0x02: region = ewram; off = addr & 0x3FFFF;     rlen = ewram_len; break;
        case 0x03: region = iwram; off = addr & 0x7FFF;      rlen = iwram_len; break;
        case 0x08: case 0x09: case 0x0A:
        case 0x0B: case 0x0C: case 0x0D:
            region = rom; off = addr & 0x01FFFFFF; rlen = rom_len; break;
        default: return nullptr;
    }
    if (!region || off > rlen || len > rlen - off) return nullptr;
    return region + off;
}

bool MemView::u8(uint32_t addr, uint8_t& out) const {
    const uint8_t* p = slice(addr, 1);
    if (!p) return false;
    out = p[0];
    return true;
}

bool MemView::u32(uint32_t addr, uint32_t& out) const {
    const uint8_t* p = slice(addr, 4);
    if (!p) return false;
    out = load_u32le(p);
    return true;
}

void Mp2kShadow::init(const std::vector<Mp2kSig>& sigs) {
    hook_n_ = 0;
    for (const auto& sig : sigs) {
        if (hook_n_ >= 4) break;
        uint32_t ram = sig.sound_main_ram;
        hook_keys_[hook_n_++] = (ram & 1) ? ram : (ram & ~3u);
    }
    active_ = true;
}

void Mp2kShadow::frame_hook(const MemView& mem, uint64_t audio_cursor, uint32_t key) {
    if (!active_ || hook_n_ == 0) return;
    ++hooks_;
    // Collapse multi-linked driver copies to the one that actually executes.
    if (hook_n_ > 1) { hook_keys_[0] = key; hook_n_ = 1; }

    uint64_t gap = audio_cursor - last_hook_cursor_;
    if (gap >= 64) {
        env_span_ = static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(gap, 256), 8192));
        env_pos_ = 0;
        last_hook_cursor_ = audio_cursor;
    }

    uint32_t si;
    if (!mem.u32(kSoundInfoPtr, si) || !(si >> 24 == 2 || si >> 24 == 3)) {
        ++stale_ticks_; return;
    }
    uint32_t ident;
    if (!mem.u32(si, ident) || ident != kMp2kMagicLive) { ++stale_ticks_; return; }
    const uint8_t* head = mem.slice(si, 0x50);
    if (!head) { ++stale_ticks_; return; }

    uint32_t master_vol = head[0x07] & 0x0F;
    reverb_ = head[0x05] & 0x7F;
    int max_chans = std::min<int>(std::max<int>(head[0x06], 1), kMp2kMaxChans);
    uint32_t pcm_freq = load_u32le(head + 0x14);
    uint32_t spv = load_u32le(head + 0x10);
    if (spv == 0 || pcm_freq == 0) { ++stale_ticks_; return; }
    dma_period_ = static_cast<uint8_t>(std::min<int>(std::max<int>(head[0x0B], 1), 16));
    mix_step_ = kRenderHz / static_cast<double>(pcm_freq);
    engaged_ = true;

    for (int ch = 0; ch < kMp2kMaxChans; ++ch) {
        Voice& v = voices_[ch];
        if (ch >= max_chans) { v.on = false; continue; }
        const uint8_t* c = mem.slice(si + kSoundChansOff + ch * kSoundChanStride, 0x28);
        if (!c) { v.on = false; continue; }
        uint8_t status = c[0x00];
        uint8_t ctype = c[0x01];
        if ((status & 0xC7u) == 0 || (ctype & 0x07u) != 0) { v.on = false; continue; }
        if (ctype & 0x10u) { v.on = false; ++bad_waves_; continue; }  // reversed: not modeled

        uint32_t vol_r = c[0x02], vol_l = c[0x03];
        uint32_t attack = c[0x04], decay = c[0x05], sustain = c[0x06], release = c[0x07];
        uint32_t env = c[0x09], echo_vol = c[0x0C];
        uint8_t  echo_len = c[0x0D];
        uint32_t count = load_u32le(c + 0x18);
        uint32_t freq = load_u32le(c + 0x20);
        uint32_t wav = load_u32le(c + 0x24);

        bool dead = false;
        uint32_t env_now = 0;
        uint32_t phase = status & 0x03u;
        bool iec = (status & 0x04u) != 0;
        bool stopping = (status & 0x40u) != 0;
        if (status & 0x80u) {  // note-on (START)
            if (stopping) {
                dead = true; env_now = 0;
            } else if (note_on(v, mem, ctype, count, wav)) {
                env_now = std::min<uint32_t>(attack, 0xFF);
                phase = 3;
                if (env_now >= 0xFF) phase = 2;
                stopping = false; iec = false;
            } else {
                ++bad_waves_; dead = true; env_now = 0;
            }
        } else if (iec) {
            env_now = env;
            if (echo_len <= 1) dead = true;
        } else if (stopping) {
            env_now = (env * release) >> 8;
            if (env_now <= echo_vol) {
                if (echo_vol == 0) dead = true;
                else { iec = true; env_now = echo_vol; }
            }
        } else {
            env_now = env;
            if (phase == 3) {
                env_now = env + attack;
                if (env_now >= 0xFF) { env_now = 0xFF; phase = 2; }
            } else if (phase == 2) {
                env_now = (env * decay) >> 8;
                if (env_now <= sustain) {
                    env_now = sustain;
                    if (sustain == 0) {
                        if (echo_vol == 0) dead = true;
                        else { iec = true; env_now = echo_vol; }
                    }
                    phase = 1;
                }
            }
        }
        if (dead) { v.on = false; continue; }

        uint32_t env_next;
        if (iec) {
            env_next = env_now;
        } else if (stopping) {
            uint32_t e = (env_now * release) >> 8;
            env_next = e <= echo_vol ? echo_vol : e;
        } else if (phase == 3) {
            env_next = std::min<uint32_t>(env_now + attack, 0xFF);
        } else if (phase == 2) {
            env_next = std::max<uint32_t>((env_now * decay) >> 8, sustain);
        } else {
            env_next = env_now;
        }

        auto gains = [&](uint32_t e, float& gr, float& gl) {
            uint32_t vv = (e * (master_vol + 1)) >> 4;
            gr = static_cast<float>((vv * vol_r) >> 8) / 256.0f;
            gl = static_cast<float>((vv * vol_l) >> 8) / 256.0f;
        };
        gains(env_now, v.g0r, v.g0l);
        gains(env_next, v.g1r, v.g1l);

        // Pitch can be rewritten mid-note (vibrato / pitch bend).
        if (ctype & 0x08u) v.step = static_cast<double>(pcm_freq) / kRenderHz;  // FIX rate
        else               v.step = static_cast<double>(freq) / kRenderHz;       // integer Hz
        v.on = true;
    }
}

void Mp2kShadow::render(const MemView& mem, int8_t canon_a, int8_t canon_b,
                        float& out_left, float& out_right, std::string& degraded) {
    degraded.clear();
    float t = std::min(static_cast<float>(env_pos_) / static_cast<float>(env_span_), 1.0f);
    if (env_pos_ != 0xFFFFFFFFu) ++env_pos_;

    float right = 0.0f, left = 0.0f;
    if (reverb_ > 0) {
        std::size_t len = ring_.size();
        std::size_t delay = static_cast<std::size_t>(dma_period_) * kFrame;
        std::size_t i0 = (ring_pos_ + len - (delay % len)) % len;
        std::size_t i1 = (i0 + 1) % len;
        float mono = (ring_[i0].first + ring_[i0].second +
                      ring_[i1].first + ring_[i1].second) *
                     (static_cast<float>(reverb_) / 512.0f);
        right = mono; left = mono;
    }

    float chk_r = right, chk_l = left;
    float gain = vf_.gain();
    for (Voice& v : voices_) {
        if (!v.on) continue;
        float s = sample_voice(v, mem);
        v.chk_acc -= 1.0;
        if (v.chk_acc <= 0.0) { v.chk_hold = s; v.chk_acc += mix_step_; }
        float gr = v.g0r + (v.g1r - v.g0r) * t;
        float gl = v.g0l + (v.g1l - v.g0l) * t;
        right += s * gr * gain;
        left += s * gl * gain;
        chk_r += v.chk_hold * v.g0r * gain;
        chk_l += v.chk_hold * v.g0l * gain;
    }

    // Canon-domain (saturated) copy feeds both the reverb ring and the
    // self-check; the clean mix is what we output.
    float sat_r = clampf(chk_r, -1.0f, 127.0f / 128.0f);
    float sat_l = clampf(chk_l, -1.0f, 127.0f / 128.0f);
    ring_[ring_pos_] = {sat_r, sat_l};
    ring_pos_ = (ring_pos_ + 1) % ring_.size();

    Judgement j = vf_.judge(static_cast<float>(canon_a) / 128.0f,
                            static_cast<float>(canon_b) / 128.0f, sat_r, sat_l);
    if (j == Judgement::Fail && vf_.last_fail_structural() && !vf_.proven()) {
        // Structure failure at a sane level while probing: search the
        // note-on `count` semantics axis (driver revisions differ); the
        // differential picks the winner. Two failing windows per mode.
        if (++mode_dwell_ >= 2) { count_mode_ ^= 1; mode_dwell_ = 0; }
    } else if (j == Judgement::Pass) {
        mode_dwell_ = 0;
    }
    std::string r = vf_.take_reverted();
    if (!r.empty()) degraded = std::move(r);

    out_left = left * 0.25f;
    out_right = right * 0.25f;
}

bool Mp2kShadow::note_on(Voice& v, const MemView& mem, uint8_t ctype,
                         uint32_t count, uint32_t wav) {
    const uint8_t* h = mem.slice(wav, 16);
    if (!h) return false;
    uint16_t flags = static_cast<uint16_t>(h[2] | (h[3] << 8));
    uint32_t loop_start = load_u32le(h + 8);
    uint32_t size = load_u32le(h + 12);
    if (size == 0 || size > 0x01000000u || loop_start > size) return false;
    bool compressed = (ctype & 0x20u) != 0;
    std::size_t bytes = compressed
        ? static_cast<std::size_t>((static_cast<uint64_t>(size) * 33 + 63) / 64)
        : static_cast<std::size_t>(size);
    if (!mem.slice(wav + 16, bytes)) return false;
    v.data = wav + 16;
    v.size = size;
    v.loop_start = loop_start;
    v.looped = (flags & 0xC000u) != 0;
    v.compressed = compressed;
    v.pos = (count_mode_ == 1) ? static_cast<double>(std::min(count, size)) : 0.0;
    v.blk_idx = 0xFFFFFFFFu;
    return true;
}

float Mp2kShadow::sample_voice(Voice& v, const MemView& mem) {
    if (v.pos >= static_cast<double>(v.size)) {
        if (v.looped && v.size > v.loop_start) {
            double span = static_cast<double>(v.size - v.loop_start);
            double over = std::fmod(v.pos - static_cast<double>(v.loop_start), span);
            v.pos = static_cast<double>(v.loop_start) + over;
        } else {
            return 0.0f;  // one-shot exhausted; envelope ends the note
        }
    }
    uint32_t i0 = static_cast<uint32_t>(v.pos);
    float frac = static_cast<float>(v.pos - static_cast<double>(i0));
    float s0 = fetch(v, mem, i0);
    uint32_t i1 = (i0 + 1 >= v.size) ? (v.looped ? v.loop_start : i0) : i0 + 1;
    float s1 = fetch(v, mem, i1);
    v.pos += v.step;
    return (s0 + (s1 - s0) * frac) / 128.0f;
}

float Mp2kShadow::fetch(Voice& v, const MemView& mem, uint32_t idx) {
    if (!v.compressed) {
        uint8_t b;
        return mem.u8(v.data + idx, b) ? static_cast<float>(static_cast<int8_t>(b)) : 0.0f;
    }
    // DPCM: 33-byte blocks of 64 samples; byte 0 is the s8 seed, then delta
    // nibbles (even k = high nibble, odd k = low; byte 1's high nibble unused).
    uint32_t blk = idx >> 6;
    if (blk != v.blk_idx) {
        const uint8_t* b = mem.slice(v.data + blk * 33, 33);
        if (!b) return 0.0f;
        int8_t cur = static_cast<int8_t>(b[0]);
        v.blk[0] = cur;
        for (int k = 1; k < 64; ++k) {
            uint8_t byte = b[1 + (k >> 1)];
            uint8_t nib = (k & 1) ? (byte & 0xF) : (byte >> 4);
            cur = static_cast<int8_t>(cur + kDpcmLut[nib]);
            v.blk[k] = cur;
        }
        v.blk_idx = blk;
    }
    return static_cast<float>(v.blk[idx & 63]);
}

}  // namespace gba
