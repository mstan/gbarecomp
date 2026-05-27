#include "host_platform.h"

#include <cstdio>
#include <thread>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <timeapi.h>  // timeBeginPeriod/timeEndPeriod (excluded by LEAN_AND_MEAN)
#  if defined(_MSC_VER)
#    pragma comment(lib, "winmm.lib")  // MinGW links winmm via CMake
#  endif
#endif

namespace gbarecomp {

FramePacer::FramePacer(double target_hz) {
    if (target_hz <= 0.0) target_hz = kGbaFrameHz;
    period_ = std::chrono::nanoseconds(
        static_cast<long long>(1e9 / target_hz + 0.5));
#if defined(_WIN32)
    // Raise the scheduler timer resolution to 1 ms so sleep_until is
    // accurate enough for ~16.74 ms frame pacing.
    if (timeBeginPeriod(1) == TIMERR_NOERROR) raised_timer_res_ = true;
#endif
    reset();
}

FramePacer::~FramePacer() {
#if defined(_WIN32)
    if (raised_timer_res_) timeEndPeriod(1);
#endif
}

void FramePacer::reset() {
    next_ = clock::now() + period_;
}

void FramePacer::wait_for_next_frame() {
    if (uncapped_) {
        next_ = clock::now();  // keep the deadline current for re-cap
        return;
    }
    clock::time_point now = clock::now();
    // If we've fallen more than one whole frame behind, resync instead
    // of trying to replay the lost time (avoids a catch-up death spiral
    // after a hitch or a breakpoint pause).
    if (now > next_ + period_) {
        next_ = now + period_;
        return;
    }
    // Hybrid wait: sleep most of the gap (cheap, but coarse), then spin
    // the final ~1 ms for deadline accuracy.
    constexpr auto kSpinTail = std::chrono::microseconds(1200);
    if (next_ - now > kSpinTail) {
        std::this_thread::sleep_until(next_ - kSpinTail);
    }
    while (clock::now() < next_) {
        std::this_thread::yield();
    }
    next_ += period_;
}

std::vector<unsigned char> read_file(const std::string& path, std::string* error) {
    std::vector<unsigned char> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "open failed: " + path;
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        if (error) *error = "ftell failed: " + path;
        std::fclose(f);
        return out;
    }
    out.resize(static_cast<std::size_t>(sz));
    std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        out.clear();
        if (error) *error = "short read: " + path;
    }
    return out;
}

}  // namespace gbarecomp
