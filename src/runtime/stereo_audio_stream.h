#pragma once
// Queued SDL playback has no callback bridge. Keep its producer-rate
// conversion state across blocks, and finish the old segment before retuning.
#include <SDL.h>
#include <cstdint>
#include <climits>
#include <vector>

namespace gbarecomp {
class StereoAudioStream {
public:
    ~StereoAudioStream() { reset(); }
    StereoAudioStream() = default;
    StereoAudioStream(const StereoAudioStream&) = delete;
    StereoAudioStream& operator=(const StereoAudioStream&) = delete;
    void reset() {
        if (stream_) SDL_FreeAudioStream(stream_);
        stream_ = nullptr;
        source_rate_ = host_rate_ = 0;
        source_frames_ = output_frames_ = 0;
        finished_ = false;
    }
    bool finish(std::vector<int16_t>& output) {
        if (!stream_ || finished_) return true;
        // SDL2 only flushes its saved resampler wing when the staging buffer
        // is nonempty. One silent frame makes that true even at an exact
        // staging boundary. Bound the result to the REAL segment duration so
        // this padding can never become an extra playback frame.
        const int16_t silence[2]{};
        if (SDL_AudioStreamPut(stream_, silence, sizeof(silence)) != 0 ||
            SDL_AudioStreamFlush(stream_) != 0) return false;
        const uint64_t duration = (source_frames_ * host_rate_ + source_rate_ - 1) / source_rate_;
        const auto start = output.size();
        const auto before = output_frames_;
        if (!drain(output)) return false;
        const auto keep = duration > before ? duration - before : 0;
        if (output_frames_ - before > keep) output.resize(start + keep * 2);
        output_frames_ = before + (output.size() - start) / 2;
        finished_ = true;
        // SDL2 can round a fractional host frame at each conversion packet.
        // A short rounded segment is valid PCM, not a failed rate transition.
        return true;
    }
    bool convert(const int16_t* pcm, std::size_t frames, uint32_t rate,
                 int host_rate, std::vector<int16_t>& output) {
        if (!pcm || !frames || frames > INT_MAX / (2 * sizeof(int16_t)) ||
            !(rate == 32768 || rate == 65536 || rate == 131072 || rate == 262144) ||
            host_rate <= 0) return false;
        if (!stream_ || finished_ || rate != source_rate_ || host_rate != host_rate_) {
            if (!finish(output)) return false;
            reset();
            stream_ = SDL_NewAudioStream(AUDIO_S16SYS, 2, int(rate),
                                        AUDIO_S16SYS, 2, host_rate);
            if (!stream_) return false;
            source_rate_ = rate; host_rate_ = host_rate;
        }
        source_frames_ += frames;
        return SDL_AudioStreamPut(stream_, pcm, int(frames * 2 * sizeof(int16_t))) == 0
            && drain(output);
    }
private:
    bool drain(std::vector<int16_t>& output) {
        const int bytes = SDL_AudioStreamAvailable(stream_);
        if (bytes < 0 || bytes % (2 * sizeof(int16_t))) return false;
        const auto start = output.size();
        output.resize(start + bytes / sizeof(int16_t));
        if (bytes && SDL_AudioStreamGet(stream_, output.data() + start, bytes) != bytes)
            return false;
        output_frames_ += bytes / (2 * sizeof(int16_t));
        return true;
    }
    SDL_AudioStream* stream_ = nullptr;
    uint32_t source_rate_ = 0;
    int host_rate_ = 0;
    uint64_t source_frames_ = 0, output_frames_ = 0;
    bool finished_ = false;
};
}
