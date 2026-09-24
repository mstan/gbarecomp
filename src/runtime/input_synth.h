// input_synth.h — frame-stepped GBA key synthesizer for host input policies.
//
// Games that translate touch into button presses must produce presses the
// guest can actually observe: a "new press" needs at least one released frame
// before the pressed frame, and some key combinations are destructive (most
// games soft-reset on A+B+Start+Select). KeySynth owns those rules so every
// game policy gets them for free. It is pure: advance with frame() exactly
// once per guest frame and feed the result to the runtime as an active-low
// KEYINPUT mask.
//
// Masks passed in are ACTIVE-HIGH GBA key bits (bit set = pressed); frame()
// returns the ACTIVE-LOW register contribution (0x03FF = nothing pressed).

#pragma once

#include <cstdint>
#include <deque>

namespace gbarecomp {

enum GbaKey : std::uint16_t {
    kGbaKeyA = 1u << 0, kGbaKeyB = 1u << 1, kGbaKeySelect = 1u << 2,
    kGbaKeyStart = 1u << 3, kGbaKeyRight = 1u << 4, kGbaKeyLeft = 1u << 5,
    kGbaKeyUp = 1u << 6, kGbaKeyDown = 1u << 7, kGbaKeyR = 1u << 8,
    kGbaKeyL = 1u << 9,
    kGbaKeyDpad = kGbaKeyRight | kGbaKeyLeft | kGbaKeyUp | kGbaKeyDown,
    kGbaKeyAll = 0x03FFu,
};

class KeySynth {
public:
    // Level-held keys stay down until released (walking direction, B-run).
    void hold(std::uint16_t keys) { held_ |= keys & kGbaKeyAll; }
    void release(std::uint16_t keys) { held_ &= static_cast<std::uint16_t>(~keys); }
    void set_held(std::uint16_t keys) { held_ = keys & kGbaKeyAll; }
    std::uint16_t held() const { return held_; }

    // Queue a discrete press: `press_frames` down, then at least
    // `release_frames` up before the next queued tap may begin. A tapped key
    // that is currently level-held is released first so the tap is an edge.
    void tap(std::uint16_t keys, int press_frames = 1, int release_frames = 1) {
        Tap t;
        t.keys = keys & kGbaKeyAll;
        t.press = press_frames < 1 ? 1 : press_frames;
        t.release = release_frames < 1 ? 1 : release_frames;
        taps_.push_back(t);
    }

    // Idle frames with nothing tapped (e.g. waiting for an input delay).
    void wait(int frames) {
        if (frames <= 0) return;
        Tap t;
        t.keys = 0;
        t.press = frames;
        t.release = 0;
        taps_.push_back(t);
    }

    void clear() {
        held_ = 0;
        taps_.clear();
        phase_ = Phase::Idle;
        remaining_ = 0;
        last_output_ = 0;
    }
    void clear_taps() {
        taps_.clear();
        phase_ = Phase::Idle;
        remaining_ = 0;
    }

    bool idle() const { return taps_.empty() && phase_ == Phase::Idle; }
    std::size_t queued() const { return taps_.size(); }

    // Keys pressed (active-high) on the previous frame() call.
    std::uint16_t last_pressed() const { return last_output_; }

    // Advance one guest frame. Returns ACTIVE-LOW KEYINPUT bits.
    std::uint16_t frame() {
        std::uint16_t pressed = held_;
        for (;;) {
            if (phase_ == Phase::Idle) {
                if (taps_.empty()) break;
                current_ = taps_.front();
                taps_.pop_front();
                // Guarantee an observable edge: if any tapped key was down on
                // the previous frame, spend one released frame first.
                if (current_.keys & last_output_) {
                    phase_ = Phase::PreRelease;
                    remaining_ = 1;
                } else {
                    phase_ = Phase::Press;
                    remaining_ = current_.press;
                }
            }
            if (phase_ == Phase::PreRelease) {
                pressed &= static_cast<std::uint16_t>(~current_.keys);
                if (--remaining_ <= 0) {
                    phase_ = Phase::Press;
                    remaining_ = current_.press;
                }
                break;
            }
            if (phase_ == Phase::Press) {
                pressed |= current_.keys;
                if (--remaining_ <= 0) {
                    phase_ = current_.release > 0 ? Phase::Release : Phase::Idle;
                    remaining_ = current_.release;
                }
                break;
            }
            if (phase_ == Phase::Release) {
                pressed &= static_cast<std::uint16_t>(~current_.keys);
                if (--remaining_ <= 0) phase_ = Phase::Idle;
                break;
            }
        }
        pressed = sanitize(pressed);
        last_output_ = pressed;
        return static_cast<std::uint16_t>(~pressed & kGbaKeyAll);
    }

    // Remove combinations no touch policy should ever emit.
    static std::uint16_t sanitize(std::uint16_t pressed) {
        constexpr std::uint16_t kReset =
            kGbaKeyA | kGbaKeyB | kGbaKeyStart | kGbaKeySelect;
        if ((pressed & kReset) == kReset)
            pressed &= static_cast<std::uint16_t>(~kGbaKeySelect);
        // Opposing directions are physically impossible on a D-pad.
        if ((pressed & (kGbaKeyLeft | kGbaKeyRight)) == (kGbaKeyLeft | kGbaKeyRight))
            pressed &= static_cast<std::uint16_t>(~(kGbaKeyLeft | kGbaKeyRight));
        if ((pressed & (kGbaKeyUp | kGbaKeyDown)) == (kGbaKeyUp | kGbaKeyDown))
            pressed &= static_cast<std::uint16_t>(~(kGbaKeyUp | kGbaKeyDown));
        return pressed;
    }

private:
    enum class Phase { Idle, PreRelease, Press, Release };
    struct Tap {
        std::uint16_t keys = 0;
        int press = 1;
        int release = 1;
    };
    std::uint16_t held_ = 0;
    std::deque<Tap> taps_;
    Phase phase_ = Phase::Idle;
    Tap current_{};
    int remaining_ = 0;
    std::uint16_t last_output_ = 0;
};

}  // namespace gbarecomp
