// pacer_test.cpp — unit test for FramePacer banked-frame throttling.
// Links src/runtime/host_platform.cpp directly; no runtime deps. Uses a
// 1 kHz pacer so waits are milliseconds, with wide bounds (timing can only
// ever overshoot, never meaningfully undershoot, the spin-covered tail).

#include "host_platform.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {
int failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("FAIL pacer: %s\n", msg); ++failures; }
}
long long now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

int main() {
    gbarecomp::FramePacer pacer(1000.0);  // 1 ms period
    const long long kBound = 10'000'000;  // 10 s: death-spiral guard

    // frames==1: exactly one period (the old behavior, unchanged).
    pacer.reset();
    long long t0 = now_us();
    pacer.wait_for_next_frame(1);
    const long long one = now_us() - t0;
    check(one >= 0 && one < kBound, "single-frame wait returns promptly");

    // Banked frames: one wait throttles all of them (4 periods ≈ 4 ms).
    pacer.reset();
    t0 = now_us();
    pacer.wait_for_next_frame(4);
    const long long banked = now_us() - t0;
    check(banked >= 2000, "banked wait throttles every banked frame");
    check(banked < kBound, "banked wait returns");

    // Defensive clamp: a huge bank cannot stall (extra capped at 15
    // periods ≈ 16 ms here, never the full bank).
    pacer.reset();
    t0 = now_us();
    pacer.wait_for_next_frame(100000);
    const long long clamped = now_us() - t0;
    check(clamped < 2'000'000, "banked wait clamps a huge bank");

    // Uncapped: immediate regardless of the bank.
    pacer.set_uncapped(true);
    t0 = now_us();
    pacer.wait_for_next_frame(4);
    check(now_us() - t0 < kBound, "uncapped wait returns immediately");

    if (failures) {
        std::printf("pacer_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("pacer_tests: OK");
    return 0;
}
