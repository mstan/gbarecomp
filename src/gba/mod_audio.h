// mod_audio.h -- bounded host-PCM overlay mixer for trusted GBA mods.
//
// This deliberately is not a second audio device and does not execute guest
// code. Trusted game code registers immutable 65536-Hz mono S16 clips during
// its static constructor/setup, then enables and controls them on the
// emulation thread. The runtime mixes active voices into the native GBA PCM
// producer stream before the normal host-volume and clock-domain bridge.
//
// Fixed-clip registration may allocate to make an owned copy of the PCM.
// Mixing, play, stop, enable, and reset never allocate, lock, or touch the SDL
// audio callback. The separate pull-stream API below is activation-authorized
// and calls its bounded source only from the emulation producer thread.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int GBAModAudioClip;
typedef uint64_t GBAModAudioStream;

#define GBA_MOD_AUDIO_CLIP_INVALID 0
#define GBA_MOD_AUDIO_STREAM_INVALID ((GBAModAudioStream)0)
#define GBA_MOD_AUDIO_SAMPLE_RATE 65536u
#define GBA_MOD_AUDIO_MAX_CLIPS 64u
#define GBA_MOD_AUDIO_MAX_VOICES 8u
#define GBA_MOD_AUDIO_MAX_STREAM_CALLBACK_FRAMES 1024u

// Pull up to max_frames of 65536-Hz mono S16 audio into dst and return the
// number produced.  This runs only on the emulation PCM-producer thread, never
// on SDL's consumer callback.  It must not block, allocate, lock, call SDL, or
// re-enter this API.  Returning fewer than max_frames ends the stream voice.
typedef size_t (*GBAModAudioStreamReadCallback)(void* context, int16_t* dst,
                                                size_t max_frames);

// Copies one immutable mono S16 PCM clip. The source must be exactly the
// engine's 65536-Hz producer rate. Returns a positive handle or INVALID.
// Newly registered clips are disabled until their trusted activation callback
// explicitly enables them.
GBAModAudioClip gba_mod_audio_register_pcm_s16_mono(
    const int16_t* samples, uint32_t frame_count, uint32_t sample_rate);

// Enable/disable a registered source. Disabling also stops its active voices.
// Returns 1 on success, 0 for an invalid handle.
int gba_mod_audio_set_enabled(GBAModAudioClip clip, int enabled);

// Starts one bounded voice. gain_percent is clamped to 0..200; loop is zero
// for a one-shot and nonzero to restart at frame zero. When every voice is
// occupied, replacement is deterministic round-robin. Returns 1 on success.
int gba_mod_audio_play(GBAModAudioClip clip, int gain_percent, int loop);

// Stop voices using one source, or all sources without unregistering them.
void gba_mod_audio_stop(GBAModAudioClip clip);
void gba_mod_audio_stop_all(void);

// Savestate-load lifecycle boundary for the runtime. It stops all fixed and
// stream delivery and restores native gain to 100, but deliberately preserves
// registered clips, stream callback, enable state, and stream capability:
// loading a savestate does not rerun plugin activation. The restored game must
// explicitly play the retained source again before it can deliver audio.
void gba_mod_audio_on_savestate_load(void);

// Restore stock lifecycle state: stop every voice and disable every source.
// The mod runtime calls this before each activation pass, so registration by
// itself has zero behavior and selected trusted plugins must opt in again.
void gba_mod_audio_reset(void);

// Register the one bounded pull-stream source for the currently executing
// selected, committed plugin activation callback. It returns an opaque,
// generation-bound capability or STREAM_INVALID; registration outside that
// engine-owned scope fails closed. Re-registering in the same activation
// replaces the source in place and retains its capability. The source starts
// disabled. Every later stream control requires this exact capability.
GBAModAudioStream gba_mod_audio_stream_register_s16_mono(
    GBAModAudioStreamReadCallback read, void* context);
int gba_mod_audio_stream_set_enabled(GBAModAudioStream stream, int enabled);
int gba_mod_audio_stream_play(GBAModAudioStream stream, int gain_percent);
int gba_mod_audio_stream_stop(GBAModAudioStream stream);

// Set the gain of guest/native PCM before fixed clips and the stream overlay
// are mixed.  It is intentionally restricted to 0..100: 0 mutes audible
// native PCM while the GBA APU/M4A remains emulated and advances normally.
// The policy is reset to 100 by gba_mod_audio_reset and savestate load.
int gba_mod_audio_stream_set_native_gain_percent(GBAModAudioStream stream,
                                                 int gain_percent);

// Runtime producer step. It invokes the optional bounded stream source only on
// the emulation producer thread, then clamps the combined native/fixed/stream
// accumulator once. It never allocates, locks, executes guest code, or touches
// SDL; SDL's consumer callback receives only the completed queue.
void gba_mod_audio_mix(int16_t* dst, size_t frame_count);

#ifdef __cplusplus
}
#endif
