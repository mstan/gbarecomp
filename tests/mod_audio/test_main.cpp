#include "mod_audio.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

void check_samples(const int16_t* got, const int16_t* want, size_t count,
                   const char* what) {
    for (size_t i = 0; i < count; ++i) {
        if (got[i] != want[i]) {
            std::printf("FAIL: %s[%zu]: got %d expected %d\n", what, i,
                        static_cast<int>(got[i]), static_cast<int>(want[i]));
            ++failures;
            return;
        }
    }
}

GBAModAudioClip register_enabled(const int16_t* samples, uint32_t frames) {
    GBAModAudioClip clip = gba_mod_audio_register_pcm_s16_mono(
        samples, frames, GBA_MOD_AUDIO_SAMPLE_RATE);
    check(clip != GBA_MOD_AUDIO_CLIP_INVALID, "registration");
    check(gba_mod_audio_set_enabled(clip, 1) == 1, "enable");
    return clip;
}

struct StreamSource {
    int16_t sample = 0;
    size_t calls = 0;
    size_t largest_request = 0;
    bool invalid_count = false;
};

size_t pull_stream(void* context, int16_t* dst, size_t max_frames) {
    auto* source = static_cast<StreamSource*>(context);
    ++source->calls;
    if (max_frames > source->largest_request) source->largest_request = max_frames;
    for (size_t i = 0; i < max_frames; ++i) dst[i] = source->sample;
    return source->invalid_count ? max_frames + 1 : max_frames;
}

void test_inactive_identity_and_bad_rate() {
    const int16_t pcm[] = {123, -456};
    std::array<int16_t, 3> dst = {7, -8, 9};
    const std::array<int16_t, 3> original = dst;
    GBAModAudioClip clip = gba_mod_audio_register_pcm_s16_mono(
        pcm, 2, GBA_MOD_AUDIO_SAMPLE_RATE);
    check(clip != GBA_MOD_AUDIO_CLIP_INVALID, "inactive registration");
    gba_mod_audio_mix(dst.data(), dst.size());
    check_samples(dst.data(), original.data(), dst.size(), "inactive identity");
    check(gba_mod_audio_register_pcm_s16_mono(pcm, 2, 32768) ==
              GBA_MOD_AUDIO_CLIP_INVALID,
          "bad rate rejected");
}

void test_one_shot_loop_and_stop_reset() {
    const int16_t pcm[] = {100, 200};
    GBAModAudioClip clip = register_enabled(pcm, 2);
    check(gba_mod_audio_play(clip, 100, 0) == 1, "one-shot starts");
    std::array<int16_t, 4> one_shot = {};
    const int16_t want_one_shot[] = {100, 200, 0, 0};
    gba_mod_audio_mix(one_shot.data(), one_shot.size());
    check_samples(one_shot.data(), want_one_shot, one_shot.size(), "one-shot");

    check(gba_mod_audio_play(clip, 100, 1) == 1, "loop starts");
    std::array<int16_t, 5> loop = {};
    const int16_t want_loop[] = {100, 200, 100, 200, 100};
    gba_mod_audio_mix(loop.data(), loop.size());
    check_samples(loop.data(), want_loop, loop.size(), "loop");

    gba_mod_audio_stop(clip);
    std::array<int16_t, 2> stopped = {};
    gba_mod_audio_mix(stopped.data(), stopped.size());
    const int16_t silence[] = {0, 0};
    check_samples(stopped.data(), silence, stopped.size(), "stop");

    check(gba_mod_audio_play(clip, 100, 1) == 1, "reset precondition");
    gba_mod_audio_reset();
    std::array<int16_t, 2> reset = {};
    gba_mod_audio_mix(reset.data(), reset.size());
    check_samples(reset.data(), silence, reset.size(), "reset stops");
    check(gba_mod_audio_play(clip, 100, 0) == 0,
          "reset disables registered source");
}

void test_saturation_and_replacement() {
    const int16_t high[] = {30000};
    GBAModAudioClip clip = register_enabled(high, 1);
    for (unsigned i = 0; i < GBA_MOD_AUDIO_MAX_VOICES; ++i)
        check(gba_mod_audio_play(clip, 200, 1) == 1, "fill voice");
    std::array<int16_t, 1> saturated = {30000};
    gba_mod_audio_mix(saturated.data(), saturated.size());
    check(saturated[0] == 32767, "saturating mix");
    gba_mod_audio_stop_all();
    std::array<int16_t, 1> stopped = {};
    gba_mod_audio_mix(stopped.data(), stopped.size());
    check(stopped[0] == 0, "stop all");

    // With all slots occupied, the ninth request deterministically replaces
    // slot zero, rather than changing the number of voices mixed this frame.
    // Different gains make the selected replacement position observable.
    const int16_t quiet[] = {100};
    GBAModAudioClip replacement_clip = register_enabled(quiet, 1);
    for (int gain = 1; gain <= 8; ++gain)
        check(gba_mod_audio_play(replacement_clip, gain, 1) == 1,
              "fill replacement voices");
    check(gba_mod_audio_play(replacement_clip, 100, 1) == 1,
          "replacement starts");
    std::array<int16_t, 1> replaced = {};
    gba_mod_audio_mix(replaced.data(), replaced.size());
    check(replaced[0] == 135,
          "round-robin replacement replaces first voice deterministically");
    gba_mod_audio_stop_all();
}

void test_producer_rates() {
    // Mod sources are 65536 Hz; the producer block follows SOUNDBIAS.
    const int16_t ramp[] = {100, 300, 500, 700, 900, 1100, 1300, 1500};
    GBAModAudioClip clip = register_enabled(ramp, 8);

    // 32768 Hz: each producer frame averages two overlay frames.
    check(gba_mod_audio_play(clip, 100, 0) == 1, "32768 play");
    std::array<int16_t, 5> slow = {10, 20, 0, 0, 0};
    const int16_t want_slow[] = {210, 620, 1000, 1400, 0};
    gba_mod_audio_mix_at_rate(slow.data(), slow.size(), 32768);
    check_samples(slow.data(), want_slow, slow.size(), "32768 averages pairs");

    // 131072 Hz: each overlay frame is held for two producer frames, and the
    // hold carries across calls.
    check(gba_mod_audio_play(clip, 100, 0) == 1, "131072 play");
    std::array<int16_t, 3> fast_a = {};
    std::array<int16_t, 3> fast_b = {};
    const int16_t want_fast_a[] = {100, 100, 300};
    const int16_t want_fast_b[] = {300, 500, 500};
    gba_mod_audio_mix_at_rate(fast_a.data(), fast_a.size(), 131072);
    gba_mod_audio_mix_at_rate(fast_b.data(), fast_b.size(), 131072);
    check_samples(fast_a.data(), want_fast_a, fast_a.size(), "131072 first call");
    check_samples(fast_b.data(), want_fast_b, fast_b.size(), "131072 carries hold");
    gba_mod_audio_stop_all();
    std::array<int16_t, 1> after_stop = {};
    gba_mod_audio_mix_at_rate(after_stop.data(), after_stop.size(), 131072);
    check(after_stop[0] == 0, "stop discards held overlay");
    gba_mod_audio_stop_all();

    // 262144 Hz: four producer frames per overlay frame.
    check(gba_mod_audio_play(clip, 100, 0) == 1, "262144 play");
    std::array<int16_t, 5> fastest = {};
    const int16_t want_fastest[] = {100, 100, 100, 100, 300};
    gba_mod_audio_mix_at_rate(fastest.data(), fastest.size(), 262144);
    check_samples(fastest.data(), want_fastest, fastest.size(), "262144 holds 4");
    gba_mod_audio_stop_all();

    // 65536 Hz (and unsupported rates) are exactly the legacy mixer.
    check(gba_mod_audio_play(clip, 100, 0) == 1, "65536 play");
    std::array<int16_t, 3> native = {};
    const int16_t want_native[] = {100, 300, 500};
    gba_mod_audio_mix_at_rate(native.data(), native.size(), 65536);
    check_samples(native.data(), want_native, native.size(), "65536 identity");
    std::array<int16_t, 2> odd = {};
    const int16_t want_odd[] = {700, 900};
    gba_mod_audio_mix_at_rate(odd.data(), odd.size(), 48000);
    check_samples(odd.data(), want_odd, odd.size(), "unsupported rate is legacy");
    gba_mod_audio_stop_all();

    // Leaving a holding rate mid-frame returns to the legacy path afterwards.
    check(gba_mod_audio_play(clip, 100, 0) == 1, "rate change play");
    std::array<int16_t, 1> held = {};
    gba_mod_audio_mix_at_rate(held.data(), held.size(), 131072);
    std::array<int16_t, 2> switched = {};
    const int16_t want_switched[] = {300, 500};
    gba_mod_audio_mix_at_rate(switched.data(), switched.size(), 65536);
    check(held[0] == 100, "held frame before rate change");
    check_samples(switched.data(), want_switched, switched.size(),
                  "rate change resumes at the next overlay frame");
    gba_mod_audio_stop_all();
}

void test_stream_rejects_untrusted_callers() {
    StreamSource source{1000};
    // Public callers have no authorization setter or plugin-ID escape hatch;
    // successful registration is covered through mod_runtime_tests.
    check(gba_mod_audio_stream_register_s16_mono(pull_stream, &source) ==
              GBA_MOD_AUDIO_STREAM_INVALID,
          "stream registration fails without activation authorization");
}

}  // namespace

int main() {
    test_inactive_identity_and_bad_rate();
    test_one_shot_loop_and_stop_reset();
    test_saturation_and_replacement();
    test_producer_rates();
    test_stream_rejects_untrusted_callers();
    if (failures) return 1;
    std::puts("GBA mod audio: bounded producer PCM mixer passed");
    return 0;
}
