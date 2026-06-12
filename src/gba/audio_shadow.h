// audio_shadow.h — engine-agnostic HLE-shadow differential verifier.
//
// This is the gate that makes the audio shadow mixer SAFE per PRINCIPLES.md
// ("Verified-enhancement HLE is permitted"). An engine shadow (MP2K, …)
// renders voices in float AND a canon-domain check copy; this class decides,
// from the running hardware mix (the canon FIFO stream), whether the shadow
// is proven enough to substitute and reacts the instant it stops matching:
//
//   - envelope correlation + level-ratio self-check vs the canon stream
//   - probation auto-gain calibration (driver revisions scale the mixer)
//   - prove-then-substitute, strike-then-pause, escalating re-prove
//
// The shadow NEVER becomes the verify oracle and reverts loudly (the caller
// logs DEGRADED) on divergence. If this verifier is wrong, the worst case is
// "we keep playing the recompiled hardware mix" — it cannot corrupt output.
//
// ── Attribution ───────────────────────────────────────────────────
// Ported from JRickey/gba-recomp (crates/gba-core/src/shadow.rs), © Jrickey,
// MIT OR Apache-2.0, used with permission. See THIRD_PARTY_ATTRIBUTION.md.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gba {

// One window's outcome, for engine-specific behavior search.
enum class Judgement {
    None,  // mid-window, or canon silent (no verdict)
    Pass,  // window passed all gates
    Fail,  // window failed (see structure_at_sane_level)
};

// Differential self-check: shadow mix vs the canon FIFO DAC stream. We police
// STRUCTURE (same notes, same loudness, same time), not raw samples: both
// streams are rectified, lowpassed to envelopes, decimated, and Pearson-
// correlated over a coarse lag range covering the driver's pipeline delay.
class ShadowSelfCheck {
public:
    // Feed one grid sample of (canon, shadow) stereo pairs. Returns true at
    // window ends that produced a verdict, filling best_r and level_ratio.
    bool push(float canon_l, float canon_r, float shadow_l, float shadow_r,
              float& best_r, float& level_ratio);

    void reset_strikes() { strikes_ = 0; }
    uint32_t strikes() const { return strikes_; }
    void add_strike() { ++strikes_; }
    void sub_strike() { if (strikes_) --strikes_; }

private:
    float lp_x_l_ = 0, lp_x_r_ = 0;  // canon envelope follower (per side)
    float lp_y_l_ = 0, lp_y_r_ = 0;  // shadow envelope follower
    std::vector<float> ex_l_, ex_r_; // decimated canon envelope
    std::vector<float> ey_l_, ey_r_; // decimated shadow envelope
    uint32_t phase_ = 0;
    uint32_t strikes_ = 0;
};

// The engine-agnostic verification state machine.
class ShadowVerifier {
public:
    // Calibrated output gain (1.0 = stock scale). Engines multiply their
    // voice sum by this in BOTH the output and the check copy.
    float gain() const { return gain_; }
    bool  proven() const { return proven_; }
    uint64_t pauses() const { return pauses_; }
    float last_r() const { return last_r_; }
    float last_ratio() const { return last_ratio_; }
    // True if the most recent verdict was a Fail whose correlation broke
    // while the level was in-band (the semantics-mismatch signature an
    // engine can search over). Only meaningful right after judge()==Fail.
    bool last_fail_structural() const { return fail_structural_; }
    // Set on each pause with the reason; caller surfaces it (DEGRADED) then
    // clears. Empty = none.
    std::string take_reverted() { std::string s = reverted_; reverted_.clear(); return s; }

    // Feed one grid sample of (canon, shadow-check-copy) pairs, both in the
    // canon's s8/128 domain. Drives calibration, proving, strikes, pauses.
    Judgement judge(float canon_l, float canon_r, float chk_l, float chk_r);

private:
    void pause(std::string reason);

    ShadowSelfCheck check_{};
    float gain_ = 1.0f;
    bool  calibrated_ = false;
    float ratio_hist_[3] = {0, 0, 0};
    uint32_t ratio_n_ = 0;
    bool  proven_ = false;
    uint32_t prove_need_ = 1;
    uint32_t consec_pass_ = 0;
    uint64_t pauses_ = 0;
    std::string reverted_;
    float last_r_ = 0;
    float last_ratio_ = 0;
    bool  fail_structural_ = false;
};

}  // namespace gba
