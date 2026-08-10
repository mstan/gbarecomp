# Trusted mod PCM overlay

`src/gba/mod_audio.h` provides a small host-side PCM overlay for trusted,
statically linked GBA game mods. It is not a guest sound driver, does not
execute guest code, and never opens a second audio device.

Fixed sources are immutable mono signed-16 PCM at 65536 Hz. Registration copies
the samples once and begins disabled. The bounded fixed mixer has 64 source
slots and 8 voices; full fixed-voice allocation uses deterministic round-robin
replacement.

The emulation thread mixes sources into the native producer PCM buffer before
the normal launcher volume and host clock-domain bridge. SDL only consumes that
already-mixed queue. Fixed-clip controls never allocate, lock, invoke
callbacks, or touch SDL. The optional pull callback runs only in the producer
mixer, never on SDL's consumer callback.

## Trusted pull stream

`gba_mod_audio_stream_register_s16_mono` adds one optional streaming mono S16
voice at the producer rate. It is for a selected, committed trusted plugin such
as a ROM-derived music interpreter; it is not a decoder or general plugin audio
device. The runtime opens an engine-private activation gate only while directly
invoking that selected callback. No public mod-audio header provides an
activation setter, constructible scope, or plugin-ID authorization API.
Registration outside that gate fails closed.

Successful registration returns an opaque nonzero `GBAModAudioStream`
capability. It is a fresh unpredictable 64-bit cookie generated during
activation, not a plugin ID or sequence number. Enable, play, stop, and native
gain all require the exact current capability. Re-registering during one
activation replaces the single source in place without consuming another slot.
Reset and reactivation retire old cookies, which are rejected if later used.

The stream voice is dedicated: it never occupies or evicts one of the eight
legacy fixed-clip voices, so their capacity and replacement behavior are
unchanged.

The pull callback receives no more than
`GBA_MOD_AUDIO_MAX_STREAM_CALLBACK_FRAMES` (1024) frames. It must be bounded:
no allocation, locks, blocking I/O, SDL use, guest execution, or re-entry into
mod audio. Returning fewer frames ends the stream voice; returning more than
requested is rejected and stops it. Mixer scratch storage is fixed.

The capability holder may set native gain to 0..100 before overlays are added.
Gain 0 mutes audible native PCM while the runtime continues draining and
advancing the guest APU/M4A. Native PCM, fixed overlays, and stream samples are
accumulated at producer width and saturated once per output frame, avoiding
sign-dependent intermediate clipping.

Activation and runtime reinitialization call `gba_mod_audio_reset`, stopping
voices, clearing callback/capability/gain state, and disabling fixed clips.
Savestate restore instead stops active delivery and restores native gain while
retaining the selected stream callback and opaque capability: loading does not
rerun plugin activation, so the restored game event must be able to play that
same source again. It never resumes automatically, so pre-load delivery is not
carried across the restore. Existing queued host audio is not flushed by this
slice.

This is a static-linked trusted-code capability boundary, not a security
sandbox against malicious same-process native code. It removes supported API
bypasses: ordinary plugins receive no activation entry point and cannot
authorize themselves by knowing another plugin's ID. A deliberately malicious
module with arbitrary process-memory access is outside this runtime's isolation
model.
