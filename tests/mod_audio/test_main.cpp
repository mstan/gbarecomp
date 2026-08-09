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

}  // namespace

int main() {
    test_inactive_identity_and_bad_rate();
    test_one_shot_loop_and_stop_reset();
    test_saturation_and_replacement();
    if (failures) return 1;
    std::puts("GBA mod audio: bounded producer PCM mixer passed");
    return 0;
}
