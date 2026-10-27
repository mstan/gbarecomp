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

static void reset_converter();

extern "C" void gba_mod_audio_stop_all(void) {
    State& s = state();
    for (Voice& voice : s.voices) clear_voice(voice);
    s.stream.playing = false;
    s.replace_cursor = 0;
    reset_converter();
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

namespace {

constexpr size_t kMixChunk = GBA_MOD_AUDIO_MAX_STREAM_CALLBACK_FRAMES;

// Render `count` (<= kMixChunk) frames of the 65536-Hz mod overlay (fixed
// voices + stream voice, gains applied, not yet saturated) into `out`.
void render_overlay(int32_t* out, size_t count) {
    State& s = state();
    std::array<int16_t, kMixChunk> stream_pcm{};
    size_t stream_count = 0;
    if (s.stream.playing && s.stream.enabled && s.stream.read) {
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
    for (size_t i = 0; i < count; ++i) out[i] = 0;
    for (Voice& voice : s.voices) {
        if (voice.clip == GBA_MOD_AUDIO_CLIP_INVALID) continue;
        ClipSlot* slot = clip_slot(voice.clip);
        if (!slot || !slot->enabled || voice.position >= slot->samples.size()) {
            clear_voice(voice);
            continue;
        }
        for (size_t i = 0; i < count; ++i) {
            out[i] += (static_cast<int32_t>(slot->samples[voice.position++]) *
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
        out[i] += (static_cast<int32_t>(stream_pcm[i]) *
                   s.stream.gain_percent) / 100;
}

// Rate conversion for a producer that is not at the 65536-Hz overlay rate.
// SOUNDBIAS only yields 32768 << k, so the ratio is an exact power of two:
// a 32768-Hz producer frame averages two overlay frames, a 131072/262144-Hz
// frame holds one overlay frame for 2/4 producer frames. Overlay frames are
// rendered only as needed, so nothing is left buffered past the call except a
// partially held frame.
struct Converter {
    std::array<int32_t, kMixChunk> overlay{};
    size_t length = 0;
    size_t position = 0;
    int32_t hold = 0;
    uint32_t hold_left = 0;
};

Converter& converter() {
    static Converter c;
    return c;
}

int32_t next_overlay_frame(size_t frames_still_needed) {
    Converter& c = converter();
    if (c.position == c.length) {
        c.length = std::min(std::max<size_t>(frames_still_needed, 1), kMixChunk);
        render_overlay(c.overlay.data(), c.length);
        c.position = 0;
    }
    return c.overlay[c.position++];
}

void mix_legacy(int16_t* dst, size_t frame_count) {
    State& s = state();
    std::array<int32_t, kMixChunk> overlay{};
    for (size_t offset = 0; offset < frame_count;) {
        const size_t count = std::min(frame_count - offset, kMixChunk);
        render_overlay(overlay.data(), count);
        for (size_t i = 0; i < count; ++i) {
            const int32_t native = (static_cast<int32_t>(dst[offset + i]) *
                                    s.native_gain_percent) / 100;
            dst[offset + i] = clamp_s16(native + overlay[i]);
        }
        offset += count;
    }
}

}  // namespace

extern "C" void gba_mod_audio_mix_at_rate(int16_t* dst, size_t frame_count,
                                          uint32_t producer_rate) {
    if (!dst || frame_count == 0) return;
    Converter& c = converter();
    const bool converter_idle = c.position == c.length && c.hold_left == 0;
    const bool supported = producer_rate == 32768u || producer_rate == 65536u ||
                           producer_rate == 131072u || producer_rate == 262144u;
    if (!supported || (producer_rate == GBA_MOD_AUDIO_SAMPLE_RATE && converter_idle)) {
        mix_legacy(dst, frame_count);
        return;
    }
    State& s = state();
    const uint32_t down = producer_rate < GBA_MOD_AUDIO_SAMPLE_RATE
        ? GBA_MOD_AUDIO_SAMPLE_RATE / producer_rate : 1u;
    const uint32_t up = producer_rate > GBA_MOD_AUDIO_SAMPLE_RATE
        ? producer_rate / GBA_MOD_AUDIO_SAMPLE_RATE : 1u;
    // A rate change away from a holding producer drops the rest of the held
    // frame (at most 3 producer frames), so the converter can go idle again.
    if (up == 1) c.hold_left = 0;
    for (size_t i = 0; i < frame_count; ++i) {
        const size_t left = frame_count - i;
        int32_t overlay = 0;
        if (up > 1) {
            if (c.hold_left == 0) {
                c.hold = next_overlay_frame((left + up - 1) / up);
                c.hold_left = up;
            }
            overlay = c.hold;
            --c.hold_left;
        } else {
            int32_t sum = 0;
            for (uint32_t k = 0; k < down; ++k)
                sum += next_overlay_frame(left * down - k);
            overlay = sum / static_cast<int32_t>(down);
        }
        const int32_t native = (static_cast<int32_t>(dst[i]) *
                                s.native_gain_percent) / 100;
        dst[i] = clamp_s16(native + overlay);
    }
}

extern "C" void gba_mod_audio_mix(int16_t* dst, size_t frame_count) {
    gba_mod_audio_mix_at_rate(dst, frame_count, GBA_MOD_AUDIO_SAMPLE_RATE);
}

// Stopping delivery also discards overlay frames rendered ahead for a
// non-65536-Hz producer, so nothing stale plays after a stop/reset/load.
static void reset_converter() { converter() = {}; }
