#pragma once

// Diagnostic-only, coarse single-emulation-thread attribution. Default builds
// contain neither timers nor branches. These phases are disjoint; HD resolution
// starts after the wide compositor's final phase has stopped.
#if defined(GBARECOMP_DETAILED_PHASE_PROFILE)
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace gba::render_profile {
enum Stage {
    NativePrepare, NativeBackground, NativeForeign, NativeObjects, NativeOutput,
    WidePrepare, WideBackground, WideForeign, WideObjects, WideOutput,
    HdInput, HdPrepare, HdRaster, HdUpscale, HdLatch, Count
};
struct Statistics {
    std::array<unsigned long long, Count> ns{}, calls{};
};
inline Statistics& statistics() { static Statistics value; return value; }
inline bool enabled() {
    static const bool value = [] {
        const char* env = std::getenv("GBARECOMP_PHASE_PROF");
        const bool on = env && !(env[0] == '0' && env[1] == '\0');
        if (on) std::atexit([] {
            constexpr const char* names[] = {
                "native-prepare", "native-background", "native-foreign", "native-objects", "native-output",
                "wide-prepare", "wide-background", "wide-foreign", "wide-objects", "wide-output",
                "hd-input", "hd-surface-palette", "hd-raster", "hd-upscale", "hd-latch"
            };
            const auto& stats = statistics();
            for (unsigned i = 0; i < Count; ++i)
                std::fprintf(stderr, "[render-phase] %s: %llu ns over %llu calls\n",
                    names[i], stats.ns[i], stats.calls[i]);
        });
        return on;
    }();
    return value;
}
class Scope {
    using Clock = std::chrono::steady_clock;
    bool active_ = enabled();
    Stage stage_;
    Clock::time_point start_;
    void record(Clock::time_point end) {
        auto& stats = statistics();
        stats.ns[stage_] += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
        ++stats.calls[stage_];
    }
public:
    explicit Scope(Stage stage) : stage_(stage) { if (active_) start_ = Clock::now(); }
    ~Scope() { stop(); }
    void mark(Stage next) {
        if (!active_) return;
        const auto end = Clock::now();
        record(end); stage_ = next; start_ = end;
    }
    void stop() {
        if (!active_) return;
        record(Clock::now()); active_ = false;
    }
};
} // namespace gba::render_profile
#define GBA_RENDER_PHASE_BEGIN(name, stage) ::gba::render_profile::Scope name(::gba::render_profile::stage)
#define GBA_RENDER_PHASE_MARK(name, stage) name.mark(::gba::render_profile::stage)
#define GBA_RENDER_PHASE_STOP(name) name.stop()
#else
#define GBA_RENDER_PHASE_BEGIN(name, stage) ((void)0)
#define GBA_RENDER_PHASE_MARK(name, stage) ((void)0)
#define GBA_RENDER_PHASE_STOP(name) ((void)0)
#endif
