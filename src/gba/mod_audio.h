// mod_audio.h -- bounded host-PCM overlay mixer for trusted GBA mods.
//
// This deliberately is not a second audio device and does not execute guest
// code. Trusted game code registers immutable 65536-Hz mono S16 clips during
// its static constructor/setup, then enables and controls them on the
// emulation thread. The runtime mixes active voices into the native GBA PCM
// producer stream before the normal host-volume and clock-domain bridge.
//
// Registration may allocate to make an owned copy of the PCM. Mixing, play,
// stop, enable, and reset never allocate, lock, invoke callbacks, or touch the
// SDL audio callback. All API calls are emulation-thread only; registration is
// intentionally not a runtime streaming API.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int GBAModAudioClip;

#define GBA_MOD_AUDIO_CLIP_INVALID 0
#define GBA_MOD_AUDIO_SAMPLE_RATE 65536u
#define GBA_MOD_AUDIO_MAX_CLIPS 64u
#define GBA_MOD_AUDIO_MAX_VOICES 8u

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

// Restore stock lifecycle state: stop every voice and disable every source.
// The mod runtime calls this before each activation pass, so registration by
// itself has zero behavior and selected trusted plugins must opt in again.
void gba_mod_audio_reset(void);

// Runtime producer step. Saturating-add active overlays into dst. This has no
// allocation, locking, callbacks, guest execution, or SDL interaction.
void gba_mod_audio_mix(int16_t* dst, size_t frame_count);

#ifdef __cplusplus
}
#endif
