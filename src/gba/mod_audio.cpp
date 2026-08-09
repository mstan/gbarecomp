// See mod_audio.h.
#include "mod_audio.h"

#include <array>
#include <limits>
#include <vector>

namespace {

struct ClipSlot {
    std::vector<int16_t> samples;
    bool enabled = false;
};

struct Voice {
    GBAModAudioClip clip = GBA_MOD_AUDIO_CLIP_INVALID;
    uint32_t position = 0;
    int gain_percent = 100;
    bool loop = false;
};

struct State {
    std::array<ClipSlot, GBA_MOD_AUDIO_MAX_CLIPS> clips;
    std::array<Voice, GBA_MOD_AUDIO_MAX_VOICES> voices;
    unsigned replace_cursor = 0;
};

// Function-local construction makes static plugin constructors safe regardless
// of cross-translation-unit initialization order.
State& state() {
    static State s;
    return s;
}

ClipSlot* clip_slot(GBAModAudioClip clip) {
    State& s = state();
    if (clip <= 0 || clip > static_cast<GBAModAudioClip>(s.clips.size()))
        return nullptr;
    ClipSlot& slot = s.clips[static_cast<size_t>(clip - 1)];
    return slot.samples.empty() ? nullptr : &slot;
}

void clear_voice(Voice& voice) {
    voice = {};
}

}  // namespace

extern "C" GBAModAudioClip gba_mod_audio_register_pcm_s16_mono(
    const int16_t* samples, uint32_t frame_count, uint32_t sample_rate) {
    if (!samples || frame_count == 0 || sample_rate != GBA_MOD_AUDIO_SAMPLE_RATE ||
        frame_count > std::numeric_limits<size_t>::max() / sizeof(*samples)) {
        return GBA_MOD_AUDIO_CLIP_INVALID;
    }

    State& s = state();
    for (size_t i = 0; i < s.clips.size(); ++i) {
        ClipSlot& slot = s.clips[i];
        if (!slot.samples.empty()) continue;
        try {
            // The sole allocation in this module. Registration is limited to
            // constructor/setup time, never the producer/audio callback path.
            slot.samples.assign(samples, samples + frame_count);
        } catch (...) {
            return GBA_MOD_AUDIO_CLIP_INVALID;
        }
        return static_cast<GBAModAudioClip>(i + 1);
    }
    return GBA_MOD_AUDIO_CLIP_INVALID;
}

extern "C" int gba_mod_audio_set_enabled(GBAModAudioClip clip, int enabled) {
    ClipSlot* slot = clip_slot(clip);
    if (!slot) return 0;
    slot->enabled = enabled != 0;
    if (!slot->enabled) gba_mod_audio_stop(clip);
    return 1;
}

extern "C" int gba_mod_audio_play(GBAModAudioClip clip, int gain_percent,
                                     int loop) {
    ClipSlot* slot = clip_slot(clip);
    if (!slot || !slot->enabled) return 0;
    if (gain_percent < 0) gain_percent = 0;
    if (gain_percent > 200) gain_percent = 200;

    State& s = state();
    size_t index = 0;
    while (index < s.voices.size() &&
           s.voices[index].clip != GBA_MOD_AUDIO_CLIP_INVALID) {
        ++index;
    }
    if (index == s.voices.size()) {
        index = s.replace_cursor;
        s.replace_cursor = (s.replace_cursor + 1u) % s.voices.size();
    }
    s.voices[index] = {clip, 0, gain_percent, loop != 0};
    return 1;
}

extern "C" void gba_mod_audio_stop(GBAModAudioClip clip) {
    for (Voice& voice : state().voices)
        if (voice.clip == clip) clear_voice(voice);
}

extern "C" void gba_mod_audio_stop_all(void) {
    State& s = state();
    for (Voice& voice : s.voices) clear_voice(voice);
    s.replace_cursor = 0;
}

extern "C" void gba_mod_audio_reset(void) {
    gba_mod_audio_stop_all();
    for (ClipSlot& slot : state().clips) slot.enabled = false;
}

extern "C" void gba_mod_audio_mix(int16_t* dst, size_t frame_count) {
    if (!dst || frame_count == 0) return;

    State& s = state();
    for (size_t i = 0; i < frame_count; ++i) {
        int32_t mixed = dst[i];
        for (Voice& voice : s.voices) {
            if (voice.clip == GBA_MOD_AUDIO_CLIP_INVALID) continue;
            ClipSlot* slot = clip_slot(voice.clip);
            if (!slot || !slot->enabled || voice.position >= slot->samples.size()) {
                clear_voice(voice);
                continue;
            }
            mixed += (static_cast<int32_t>(slot->samples[voice.position++]) *
                      voice.gain_percent) / 100;
            if (voice.position == slot->samples.size()) {
                if (voice.loop) voice.position = 0;
                else clear_voice(voice);
            }
        }
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        dst[i] = static_cast<int16_t>(mixed);
    }
}
