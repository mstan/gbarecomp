#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

extern "C" {
int dsp_init(int host);
void dsp_free();
void dsp_reset(int index);
int16_t* dsp_input();
int16_t* dsp_output();
void dsp_push(int n);
int dsp_pull(int n, int ending);
double dsp_fill();
}

namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) { std::printf("FAIL: %s\n", what); ++failures; }
}

void test_segments(int host) {
    if (dsp_init(host)) { check(false, "DSP initialization"); return; }
    for (int index = 0; index < 4; ++index) {
        dsp_reset(index);
        check(dsp_fill() == 0, "rate bank starts empty");
        dsp_push(0);
        dsp_push(2049);
        check(dsp_fill() == 0, "invalid pushes leave queue empty");
        check(dsp_pull(0, 0) == 0 && dsp_pull(2049, 0) == 0,
              "invalid pulls rejected");
        std::fill_n(dsp_input(), 1024, int16_t{12000});
        dsp_push(1024);
        // Short segments are below preroll, but ending must drain them in
        // exactly their duration, without waiting for more source samples.
        const int expected = static_cast<int>(std::ceil(1024.0 * host / (32768u << index)));
        int frames = 0, peak = 0;
        for (int budget = 0; budget < 64; ++budget) {
            const int n = dsp_pull(128, 1);
            check(n >= 0 && n <= 128, "pull stays within output capacity");
            if (n <= 0 || n > 128) break;
            for (int i = 0; i < n; ++i)
                peak = std::max(peak, std::abs(int(dsp_output()[i])));
            frames += n;
        }
        check(frames == expected, "segment duration follows source and host rates");
        check(peak > 0, "short ending segment produces audible PCM");
        check(dsp_pull(128, 1) == 0, "ended segment cannot emit endless concealment");
        dsp_reset(index);
        check(dsp_fill() == 0 && dsp_pull(128, 1) == 0,
              "reset removes previous segment duration and PCM");
        std::fill_n(dsp_output(), 128, int16_t{1234});
        check(dsp_pull(128, 0) == 128, "empty running segment returns a full quantum");
        check(std::all_of(dsp_output(), dsp_output() + 128,
                          [](int16_t sample) { return sample == 0; }),
              "unprimed bank emits silence");
    }
    dsp_free();
    dsp_free();  // Teardown is also used on partially initialized worklets.
}
}  // namespace

int main() {
    test_segments(44100);
    test_segments(48000);
    if (failures) return 1;
    std::puts("web audio DSP: four rates, segment duration and reset passed");
    return 0;
}
