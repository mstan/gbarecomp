// See mod_audio.h.
#include "mod_audio.h"

#include <algorithm>
#include <array>
#include <limits>
#include <random>
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

struct StreamSlot {
    GBAModAudioStream capability = GBA_MOD_AUDIO_STREAM_INVALID;
    GBAModAudioStreamReadCallback read = nullptr;
    void* context = nullptr;
    uint32_t activation_epoch = 0;
    int gain_percent = 100;
    bool enabled = false;
    bool playing = false;
};

struct State {
    std::array<ClipSlot, GBA_MOD_AUDIO_MAX_CLIPS> clips;
    // This is intentionally the legacy fixed-clip pool only. The single
    // streaming voice below must never consume or replace one of these slots.
    std::array<Voice, GBA_MOD_AUDIO_MAX_VOICES> voices;
    unsigned replace_cursor = 0;
    StreamSlot stream;
    uint32_t activation_depth = 0;
    uint32_t activation_epoch = 0;
    std::array<GBAModAudioStream, 64> retired_capabilities{};
    size_t retired_cursor = 0;
    int native_gain_percent = 100;
};

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

void clear_voice(Voice& voice) { voice = {}; }

void invalidate_stream() {
    State& s = state();
    if (s.stream.capability != GBA_MOD_AUDIO_STREAM_INVALID) {
        s.retired_capabilities[s.retired_cursor] = s.stream.capability;
        s.retired_cursor = (s.retired_cursor + 1u) % s.retired_capabilities.size();
    }
    s.stream = {};
    s.native_gain_percent = 100;
}

uint32_t next_nonzero(uint32_t& value) {
    ++value;
    if (value == 0) ++value;
    return value;
}

bool retired_capability(GBAModAudioStream candidate) {
    for (GBAModAudioStream retired : state().retired_capabilities)
        if (candidate == retired) return true;
    return false;
}

GBAModAudioStream make_capability() {
    // This runs during activation, never in the producer mixer. Do not fall
    // back to a predictable counter if the OS-backed random source fails.
    try {
        std::random_device random;
        for (unsigned attempt = 0; attempt != 32; ++attempt) {
            const GBAModAudioStream candidate =
                (static_cast<GBAModAudioStream>(random()) << 32) | random();
            if (candidate > UINT64_C(0xffff) &&
                candidate != state().stream.capability &&
                !retired_capability(candidate)) return candidate;
        }
    } catch (...) {
    }
    return GBA_MOD_AUDIO_STREAM_INVALID;
}

bool valid_stream(GBAModAudioStream stream) {
    const StreamSlot& slot = state().stream;
    return stream != GBA_MOD_AUDIO_STREAM_INVALID &&
           stream == slot.capability && slot.read != nullptr;
}

Voice* allocate_fixed_voice() {
    State& s = state();
    for (Voice& voice : s.voices)
        if (voice.clip == GBA_MOD_AUDIO_CLIP_INVALID) return &voice;
    Voice* voice = &s.voices[s.replace_cursor];
    s.replace_cursor = (s.replace_cursor + 1u) % s.voices.size();
    return voice;
}

int16_t clamp_s16(int32_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return static_cast<int16_t>(value);
}

}  // namespace

namespace gbarecomp::mod_audio_engine_private {

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
__attribute__((visibility("hidden")))
#endif
void begin_stream_activation() {
    State& s = state();
    if (s.activation_depth++ == 0) next_nonzero(s.activation_epoch);
}

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
__attribute__((visibility("hidden")))
#endif
void end_stream_activation() {
    State& s = state();
    if (s.activation_depth) --s.activation_depth;
}

}  // namespace gbarecomp::mod_audio_engine_private

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
    Voice* voice = allocate_fixed_voice();
    *voice = {clip, 0, gain_percent, loop != 0};
    return 1;
}

extern "C" void gba_mod_audio_stop(GBAModAudioClip clip) {
    for (Voice& voice : state().voices)
        if (voice.clip == clip) clear_voice(voice);
}

extern "C" void gba_mod_audio_stop_all(void) {
    State& s = state();
    for (Voice& voice : s.voices) clear_voice(voice);
    s.stream.playing = false;
    s.replace_cursor = 0;
}

extern "C" void gba_mod_audio_on_savestate_load(void) {
    // A load does not rerun mod activation, so this must retain the selected
    // stream callback and its opaque capability. It only discards delivery
    // state; the restored guest event explicitly starts the retained source.
    gba_mod_audio_stop_all();
    state().native_gain_percent = 100;
}

extern "C" void gba_mod_audio_reset(void) {
    State& s = state();
    gba_mod_audio_stop_all();
    for (ClipSlot& slot : s.clips) slot.enabled = false;
    invalidate_stream();
    // An in-flight activation cannot survive an externally initiated reset.
    s.activation_depth = 0;
    next_nonzero(s.activation_epoch);
}

extern "C" GBAModAudioStream gba_mod_audio_stream_register_s16_mono(
    GBAModAudioStreamReadCallback read, void* context) {
    State& s = state();
    if (!read || s.activation_depth == 0) return GBA_MOD_AUDIO_STREAM_INVALID;
    if (s.stream.capability != GBA_MOD_AUDIO_STREAM_INVALID &&
        s.stream.activation_epoch != s.activation_epoch) {
        // One activation pass admits one selected plugin source. A later
        // committed plugin callback cannot steal it; a reset/reactivation is
        // the only way to issue a new capability.
        return GBA_MOD_AUDIO_STREAM_INVALID;
    }
    if (s.stream.capability == GBA_MOD_AUDIO_STREAM_INVALID) {
        invalidate_stream();
        s.stream.capability = make_capability();
        if (s.stream.capability == GBA_MOD_AUDIO_STREAM_INVALID)
            return GBA_MOD_AUDIO_STREAM_INVALID;
        s.stream.activation_epoch = s.activation_epoch;
    }
    s.stream.read = read;
    s.stream.context = context;
    s.stream.enabled = false;
    s.stream.playing = false;
    s.stream.gain_percent = 100;
    return s.stream.capability;
}

extern "C" int gba_mod_audio_stream_set_enabled(GBAModAudioStream stream,
                                                   int enabled) {
    if (!valid_stream(stream)) return 0;
    State& s = state();
    s.stream.enabled = enabled != 0;
    if (!s.stream.enabled) s.stream.playing = false;
    return 1;
}

extern "C" int gba_mod_audio_stream_play(GBAModAudioStream stream,
                                            int gain_percent) {
    if (!valid_stream(stream) || !state().stream.enabled) return 0;
    if (gain_percent < 0) gain_percent = 0;
    if (gain_percent > 200) gain_percent = 200;
    state().stream.gain_percent = gain_percent;
    state().stream.playing = true;
    return 1;
}

extern "C" int gba_mod_audio_stream_stop(GBAModAudioStream stream) {
    if (!valid_stream(stream)) return 0;
    state().stream.playing = false;
    return 1;
}

extern "C" int gba_mod_audio_stream_set_native_gain_percent(
    GBAModAudioStream stream, int gain_percent) {
    if (!valid_stream(stream) || gain_percent < 0 || gain_percent > 100)
        return 0;
    state().native_gain_percent = gain_percent;
    return 1;
}

extern "C" void gba_mod_audio_mix(int16_t* dst, size_t frame_count) {
    if (!dst || frame_count == 0) return;
    State& s = state();
    std::array<int16_t, GBA_MOD_AUDIO_MAX_STREAM_CALLBACK_FRAMES> stream_pcm{};
    std::array<int32_t, GBA_MOD_AUDIO_MAX_STREAM_CALLBACK_FRAMES> mixed{};

    for (size_t offset = 0; offset < frame_count;) {
        const size_t count = std::min(
            frame_count - offset,
            static_cast<size_t>(GBA_MOD_AUDIO_MAX_STREAM_CALLBACK_FRAMES));
        size_t stream_count = 0;
        if (s.stream.playing && s.stream.enabled && s.stream.read) {
            stream_pcm.fill(0);
            stream_count = s.stream.read(s.stream.context, stream_pcm.data(), count);
            if (stream_count > count) {
                // A malformed trusted callback fails closed before contributing
                // to this buffer or any later producer buffer.
                s.stream.playing = false;
                stream_count = 0;
            } else if (stream_count != count) {
                s.stream.playing = false;
            }
        }
        for (size_t i = 0; i < count; ++i) {
            mixed[i] = (static_cast<int32_t>(dst[offset + i]) *
                        s.native_gain_percent) / 100;
        }
        for (Voice& voice : s.voices) {
            if (voice.clip == GBA_MOD_AUDIO_CLIP_INVALID) continue;
            ClipSlot* slot = clip_slot(voice.clip);
            if (!slot || !slot->enabled || voice.position >= slot->samples.size()) {
                clear_voice(voice);
                continue;
            }
            for (size_t i = 0; i < count; ++i) {
                mixed[i] += (static_cast<int32_t>(slot->samples[voice.position++]) *
                             voice.gain_percent) / 100;
                if (voice.position == slot->samples.size()) {
                    if (voice.loop) voice.position = 0;
                    else {
                        clear_voice(voice);
                        break;
                    }
                }
            }
        }
        for (size_t i = 0; i < stream_count; ++i)
            mixed[i] += (static_cast<int32_t>(stream_pcm[i]) *
                         s.stream.gain_percent) / 100;
        for (size_t i = 0; i < count; ++i)
            dst[offset + i] = clamp_s16(mixed[i]);
        offset += count;
    }
}
