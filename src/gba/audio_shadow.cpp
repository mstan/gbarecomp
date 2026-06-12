// audio_shadow.cpp — see audio_shadow.h.
//
// Ported from JRickey/gba-recomp (crates/gba-core/src/shadow.rs), © Jrickey,
// MIT OR Apache-2.0, used with permission. See THIRD_PARTY_ATTRIBUTION.md.

#include "audio_shadow.h"

#include <cmath>
#include <cstdio>

namespace gba {
namespace {

constexpr uint32_t kDecim        = 64;     // one envelope entry per ~1 ms
constexpr std::size_t kWindow    = 1024;   // ~1 s window
constexpr std::size_t kMaxLag    = 56;     // ~3.5 driver frames, step 2
constexpr float kEnvAlpha        = 0.0075f;
constexpr double kMinLevel       = 0.002;
constexpr float kMinR            = 0.5f;
constexpr uint32_t kMaxStrikes   = 3;

double mean_of(const std::vector<float>& v) {
    double s = 0;
    for (float x : v) s += x;
    return v.empty() ? 0.0 : s / static_cast<double>(v.size());
}

}  // namespace

bool ShadowSelfCheck::push(float canon_l, float canon_r, float shadow_l,
                           float shadow_r, float& best_r, float& level_ratio) {
    lp_x_l_ += kEnvAlpha * (std::fabs(canon_l) - lp_x_l_);
    lp_x_r_ += kEnvAlpha * (std::fabs(canon_r) - lp_x_r_);
    lp_y_l_ += kEnvAlpha * (std::fabs(shadow_l) - lp_y_l_);
    lp_y_r_ += kEnvAlpha * (std::fabs(shadow_r) - lp_y_r_);
    if (++phase_ < kDecim) return false;
    phase_ = 0;
    ex_l_.push_back(lp_x_l_);
    ex_r_.push_back(lp_x_r_);
    ey_l_.push_back(lp_y_l_);
    ey_r_.push_back(lp_y_r_);
    if (ex_l_.size() < kWindow) return false;

    const std::size_t n = kWindow - kMaxLag;
    double m0 = mean_of(ex_l_), m1 = mean_of(ex_r_);
    double canon_level  = (m0 + m1) / 2.0;
    double shadow_level = (mean_of(ey_l_) + mean_of(ey_r_)) / 2.0;

    bool have_verdict = false;
    if (canon_level >= kMinLevel) {
        float ratio = static_cast<float>(shadow_level / canon_level);
        // Does the canon envelope carry structure this window (vs a held
        // constant amplitude)?
        double canon_var = 0;
        for (std::size_t i = 0; i < ex_l_.size(); ++i) {
            double a = ex_l_[i] - m0, b = ex_r_[i] - m1;
            canon_var += a * a + b * b;
        }
        canon_var /= static_cast<double>(ex_l_.size());

        float best = 0.0f;
        if (canon_var <= 1e-10) {
            // Flat canon: nothing to correlate — the ratio is the verdict.
            best = 1.0f;
        } else {
            bool any = false;
            for (std::size_t lag = 0; lag <= kMaxLag; lag += 2) {
                double sum = 0;
                int sides = 0;
                for (int side = 0; side < 2; ++side) {
                    const std::vector<float>& xv = side == 0 ? ex_l_ : ex_r_;
                    const std::vector<float>& yv = side == 0 ? ey_l_ : ey_r_;
                    // x = ex[kMaxLag .. kWindow); y = ey[kMaxLag-lag .. kWindow-lag).
                    double mx = 0, my = 0;
                    for (std::size_t i = 0; i < n; ++i) {
                        mx += xv[kMaxLag + i];
                        my += yv[kMaxLag - lag + i];
                    }
                    mx /= static_cast<double>(n);
                    my /= static_cast<double>(n);
                    double cov = 0, vx = 0, vy = 0;
                    for (std::size_t i = 0; i < n; ++i) {
                        double a = xv[kMaxLag + i] - mx;
                        double b = yv[kMaxLag - lag + i] - my;
                        cov += a * b;
                        vx += a * a;
                        vy += b * b;
                    }
                    if (vx > 1e-12 && vy > 1e-12) {
                        sum += cov / std::sqrt(vx * vy);
                        ++sides;
                    }
                }
                if (sides > 0) {
                    float r = static_cast<float>(sum / sides);
                    if (!any || r > best) best = r;
                    any = true;
                }
            }
            if (!any) best = 0.0f;
        }
        best_r = best;
        level_ratio = ratio;
        have_verdict = true;
    }

    ex_l_.clear(); ex_r_.clear();
    ey_l_.clear(); ey_r_.clear();
    return have_verdict;
}

void ShadowVerifier::pause(std::string reason) {
    proven_ = false;
    consec_pass_ = 0;
    check_.reset_strikes();
    prove_need_ = prove_need_ * 2 < 16 ? prove_need_ * 2 : 16;
    ++pauses_;
    reverted_ = std::move(reason);
}

Judgement ShadowVerifier::judge(float canon_l, float canon_r,
                                float chk_l, float chk_r) {
    float r, ratio;
    if (!check_.push(canon_l, canon_r, chk_l, chk_r, r, ratio))
        return Judgement::None;
    last_r_ = r;
    last_ratio_ = ratio;

    // Probation auto-calibration: strong structure with a stable off-band
    // level means this driver revision scales its mixer by a constant —
    // adopt it instead of striking.
    if (!calibrated_ && r >= 0.7f && !(ratio >= 0.85f && ratio <= 1.15f) &&
        (ratio >= 0.2f && ratio <= 5.0f) && ratio_n_ < 6) {
        ratio_hist_[ratio_n_ % 3] = ratio;
        ++ratio_n_;
        if (ratio_n_ >= 3) {
            float m = (ratio_hist_[0] + ratio_hist_[1] + ratio_hist_[2]) / 3.0f;
            bool stable = true;
            for (float x : ratio_hist_)
                if (std::fabs(x / m - 1.0f) >= 0.1f) stable = false;
            if (stable) {
                float g = gain_ / m;
                gain_ = g < 0.25f ? 0.25f : (g > 4.0f ? 4.0f : g);
                calibrated_ = true;
                check_.reset_strikes();
            }
        }
        return Judgement::None;
    }

    // Two failure axes: structure (correlation) and level (envelope ratio —
    // scale-invariant correlation alone would bless wrong volume math).
    bool level_ok = ratio >= 0.55f && ratio <= 1.6f;
    bool window_ok = r >= kMinR && level_ok;
    if (proven_) {
        if (window_ok) {
            check_.sub_strike();
        } else {
            check_.add_strike();
            if (check_.strikes() >= kMaxStrikes) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "shadow/canon correlation %.2f, level ratio %.2f "
                              "(driver variant or unsupported feature)", r, ratio);
                pause(buf);
            }
        }
    } else if (window_ok) {
        ++consec_pass_;
        if (consec_pass_ >= prove_need_) {
            proven_ = true;
            check_.reset_strikes();
        }
    } else {
        consec_pass_ = 0;
    }
    fail_structural_ = !window_ok && (r < kMinR && level_ok);
    return window_ok ? Judgement::Pass : Judgement::Fail;
}

}  // namespace gba
