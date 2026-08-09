# Trusted mod PCM overlay

`src/gba/mod_audio.h` provides a deliberately small host-side PCM overlay for
trusted, statically linked GBA game mods. It is not a guest sound driver,
doesn't execute guest code, and never opens a second audio device.

Sources are immutable mono signed-16 PCM at 65536 Hz. A source is registered
at constructor/setup time, which copies its samples once, and begins disabled.
The selected trusted activation callback must enable it before it can play.
The bounded mixer has 64 source slots and 8 voices; full voice allocation uses
deterministic round-robin replacement.

The emulation thread mixes sources into the native producer PCM buffer before
the normal launcher volume and host clock-domain bridge. The SDL callback only
consumes that already-mixed stream. Therefore `mix`, `play`, `stop`, `enable`,
and `reset` allocate nothing, take no locks, and invoke no callbacks.

Savestate loads stop overlay voices instead of serializing their cursors. This
prevents a host cue started before a load from continuing over the restored
world; a restored game event must explicitly play it again. Existing queued
host audio is not flushed by this first slice.

This API intentionally has no native MP2K duck/mute policy. MP2K and other
audio share Direct Sound A/B, so a correct music-only policy requires a future
GBA mixer split plus game-owned ownership semantics.
