#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"

int main() {
    rab_config cfg;
    rab_config_defaults(&cfg);
    cfg.channels = 1;
    cfg.source_rate = 48000.0;
    cfg.host_rate = 48000.0;
    cfg.target_ms = 10.0;
    cfg.ring_ms = 100.0;
    cfg.preroll_ms = 10.0;
    cfg.stretch_limit_ms = 20.0;

    rab_bridge bridge{};
    if (rab_init(&bridge, &cfg) != 0) {
        std::fprintf(stderr, "rab_init failed\n");
        return 1;
    }

    std::vector<int16_t> input(2400);
    for (std::size_t i = 0; i < input.size(); ++i) {
        constexpr double kPi = 3.14159265358979323846;
        input[i] = static_cast<int16_t>(
            std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) /
                     cfg.source_rate) *
            12000.0);
    }
    rab_push(&bridge, input.data(), static_cast<int>(input.size()));

    std::vector<int16_t> output(6000);
    rab_pull(&bridge, output.data(), static_cast<int>(output.size()));

    rab_stats stats{};
    rab_get_stats(&bridge, &stats);
    const uint64_t limit_frames =
        static_cast<uint64_t>(cfg.stretch_limit_ms * cfg.host_rate / 1000.0);
    if (stats.stretch_frames == 0 || stats.stretch_frames > limit_frames) {
        std::fprintf(stderr,
                     "stall concealment exceeded limit: stretch=%llu limit=%llu\n",
                     static_cast<unsigned long long>(stats.stretch_frames),
                     static_cast<unsigned long long>(limit_frames));
        rab_free(&bridge);
        return 2;
    }

    const auto tail_begin = output.end() - 256;
    const int16_t tail_peak = *std::max_element(
        tail_begin, output.end(),
        [](int16_t a, int16_t b) { return std::abs(a) < std::abs(b); });
    if (std::abs(static_cast<int>(tail_peak)) > 1) {
        std::fprintf(stderr, "stalled output did not fade to silence: peak=%d\n",
                     static_cast<int>(tail_peak));
        rab_free(&bridge);
        return 3;
    }

    rab_free(&bridge);
    return 0;
}
