#pragma once
#include <algorithm>
#include <chrono>

namespace gbarecomp {
// Presentation/admission pacing only; this clock never enters guest state.
// Account for execution time instead of sleeping a whole frame after running
// one. Brief jitter retains the schedule (up to four frames, like the shared
// ecosystem's NES runner); long stalls reset it. Replay consumes no new slot.
class GbaNetplayPacer {
public:
    GbaNetplayPacer() = default;
    ~GbaNetplayPacer();
    GbaNetplayPacer(const GbaNetplayPacer&) = delete;
    GbaNetplayPacer& operator=(const GbaNetplayPacer&) = delete;
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr auto frame_period = std::chrono::nanoseconds(
        280896LL * 1000000000LL / 16777216LL);
    bool ready(Time now) const { return now >= next_; }
    // Yield for about 1 ms while servicing the network between frames. A
    // high-resolution wait on Windows avoids Sleep(1)'s ~16 ms timer quantum.
    void idle();
    void forwarded(Time frame_start) {
        if (next_==Time{} || frame_start>next_+frame_period*4) next_=frame_start;
        next_+=frame_period;
    }
private:
    Time next_{};
    void* wake_timer_ = nullptr;
    bool tried_timer_ = false;
};
}
