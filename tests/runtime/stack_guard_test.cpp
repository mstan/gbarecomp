// stack_guard_test.cpp — unit test for the host-stack budget guard
// (mobile_platform.h). Links src/runtime/mobile_platform.cpp directly; no
// runtime deps. On Linux it arms the guard for the test thread and proves a
// deep probe trips the budget while staying far above the critical tier,
// via bounded recursion with live 4 KiB frames.

#include "mobile_platform.h"

#include <cstddef>
#include <cstdio>
#include <future>
#include <thread>

namespace {
int failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("FAIL stack_guard: %s\n", msg); ++failures; }
}

// Recurse to `target`, then probe. noinline + volatile frames keep the
// compiler from collapsing the descent into a loop.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool descend(unsigned depth, unsigned target) {
    volatile char pad[4096];
    pad[0] = static_cast<char>(depth);
    pad[sizeof(pad) - 1] = static_cast<char>(depth >> 8);
    if (depth < target) {
        // Non-tail call: pad stays live across it, so each level keeps a
        // real frame (a tail call would reuse one frame forever).
        const bool inner = descend(depth + 1, target);
        return inner && pad[0] == static_cast<char>(depth);
    }
    char probe;
    const bool over = gbarecomp::mobile_stack_over_budget(&probe);
    const bool crit = gbarecomp::host_stack_critically_low(&probe);
    return over && !crit && pad[0] == static_cast<char>(depth) &&
           pad[sizeof(pad) - 1] == static_cast<char>(depth >> 8);
}
}  // namespace

int main() {
    char top;
    // Unarmed: both tiers always read false (generous stacks, main path).
    check(!gbarecomp::mobile_stack_over_budget(&top),
          "unarmed budget reads false");
    check(!gbarecomp::host_stack_critically_low(&top),
          "unarmed critical tier reads false");
#if defined(__linux__)
    std::promise<void> armed, release;
    auto ready = armed.get_future();
    auto released = release.get_future();
    bool worker_armed = false;
    std::thread worker([&] {
        gbarecomp::host_stack_guard_arm_current(4u * 1024u * 1024u);
        worker_armed = gbarecomp::mobile_stack_over_budget(reinterpret_cast<void*>(1));
        armed.set_value();
        released.wait();
    });
    ready.wait();
    check(worker_armed, "worker guard is armed");
    check(!gbarecomp::mobile_stack_over_budget(reinterpret_cast<void*>(1)),
          "worker stack bounds must not arm another thread");
    check(!gbarecomp::host_stack_critically_low(reinterpret_cast<void*>(1)),
          "worker critical tier must not leak to another thread");
    release.set_value();
    worker.join();
    // Arm a 4 MiB reserve, then consume past the budget line (~1500 live
    // 4 KiB frames ≈ 6 MiB) while remaining megabytes above the critical
    // tier. This is the shape the runtime relies on: unwind through the
    // yield path before the finite-stack thread is exhausted.
    gbarecomp::host_stack_guard_arm_current(4u * 1024u * 1024u);
    check(!gbarecomp::mobile_stack_over_budget(&top),
          "armed budget still false at the top of the stack");
    check(!gbarecomp::host_stack_critically_low(&top),
          "armed critical tier still false at the top of the stack");
    check(descend(0, 1500),
          "deep probe exceeds the budget before the critical tier");
#else
    std::puts("stack_guard_tests: SKIP (needs Linux pthread stack bounds)");
#endif
    if (failures) {
        std::printf("stack_guard_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("stack_guard_tests: OK");
    return 0;
}
