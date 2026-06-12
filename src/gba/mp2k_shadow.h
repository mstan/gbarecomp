// mp2k_shadow.h — MP2K ("m4a") HLE shadow mixer (the float re-render).
//
// QoL-on-top-of-correctness per PRINCIPLES.md "Verified-enhancement HLE":
// the recompiled guest M4A driver still runs and still produces the canon
// FIFO stream (which stays the verify oracle). This shadow re-renders the
// SAME voice state in float at 65536 Hz — free of the driver's 8-bit
// requantization and low mix-rate ceiling — and is policed every grid
// sample by the engine-agnostic ShadowVerifier (audio_shadow.h). It
// substitutes only after a proven window and reverts loudly (DEGRADED).
//
// All guest reads are side-effect-free (region pointers, no bus I/O, no
// waitstates), so rendering never perturbs emulation.
//
// ── Attribution ───────────────────────────────────────────────────
// Ported from JRickey/gba-recomp (crates/gba-core/src/mp2k.rs), © Jrickey,
// MIT OR Apache-2.0, used with permission. C++ port is ours; the engine
// struct map is shared with gba_m4a.h. See THIRD_PARTY_ATTRIBUTION.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio_shadow.h"
#include "gba_m4a.h"

namespace gba {

// Read-only view over the regions sample data can live in. Side-effect free.
struct MemView {
    const uint8_t* rom = nullptr;    std::size_t rom_len = 0;
    const uint8_t* ewram = nullptr;  std::size_t ewram_len = 0;
    const uint8_t* iwram = nullptr;  std::size_t iwram_len = 0;

    // Resolve [addr, addr+len) to a pointer, or nullptr if it leaves region.
    const uint8_t* slice(uint32_t addr, std::size_t len) const;
    bool  u8(uint32_t addr, uint8_t& out) const;
    bool  u32(uint32_t addr, uint32_t& out) const;
};

class Mp2kShadow {
public:
    // Arm the shadow from detected MP2K signatures (SoundMainRAM hook keys).
    void init(const std::vector<Mp2kSig>& sigs);
    bool armed() const { return hook_n_ > 0; }

    // True while the frontend should substitute the shadow stream: armed,
    // engaged (saw a live tick), and the verifier has proven a window.
    bool live() const { return active_ && engaged_ && vf_.proven(); }

    // Per driver-tick hook: re-read the finalized channel state and mirror
    // the envelope step to derive this tick's gains + a one-tick prediction.
    // `audio_cursor` is a running grid-sample count; `key` is the detected
    // hook (used only to collapse multi-linked driver copies).
    void frame_hook(const MemView& mem, uint64_t audio_cursor, uint32_t key);

    // Render one grid sample. `canon` is the live FIFO DAC pair (A, B) for
    // the differential self-check. Returns (left, right) in bus float scale
    // (full-scale FIFO DAC = 0.25). Drives the verifier; sets `degraded` to
    // a reason string when it just reverted (caller logs it), else empty.
    void render(const MemView& mem, int8_t canon_a, int8_t canon_b,
                float& out_left, float& out_right, std::string& degraded);

    uint32_t hook_key0() const { return hook_n_ ? hook_keys_[0] : 0; }

private:
    struct Voice {
        bool     on = false;
        uint32_t data = 0;
        uint32_t size = 0;
        uint32_t loop_start = 0;
        bool     looped = false;
        bool     compressed = false;
        double   pos = 0.0;
        double   step = 0.0;
        float    g0r = 0, g0l = 0;   // gains at tick start
        float    g1r = 0, g1l = 0;   // one-tick prediction
        uint32_t blk_idx = 0xFFFFFFFFu;
        int8_t   blk[64] = {};
        float    chk_hold = 0.0f;    // canon-domain check-copy hold
        double   chk_acc = 0.0;
    };

    bool note_on(Voice& v, const MemView& mem, uint8_t ctype, uint32_t count,
                 uint32_t wav);
    float sample_voice(Voice& v, const MemView& mem);
    float fetch(Voice& v, const MemView& mem, uint32_t idx);

    static constexpr double kRenderHz = 65536.0;
    static constexpr std::size_t kFrame = 1097;  // grid samples per 59.7 Hz tick

    uint32_t hook_keys_[4] = {};
    uint8_t  hook_n_ = 0;
    bool     active_ = true;
    bool     engaged_ = false;
    uint64_t hooks_ = 0, stale_ticks_ = 0, bad_waves_ = 0;
    std::array<Voice, kMp2kMaxChans> voices_{};
    uint32_t env_pos_ = 0;
    uint32_t env_span_ = static_cast<uint32_t>(kFrame);
    uint64_t last_hook_cursor_ = 0;
    uint8_t  reverb_ = 0;
    uint8_t  dma_period_ = 7;
    std::vector<std::pair<float, float>> ring_ =
        std::vector<std::pair<float, float>>(kFrame * 16, {0.0f, 0.0f});
    std::size_t ring_pos_ = 0;
    ShadowVerifier vf_{};
    uint8_t  count_mode_ = 0;
    uint32_t mode_dwell_ = 0;
    double   mix_step_ = 65536.0 / 13379.0;
};

}  // namespace gba
