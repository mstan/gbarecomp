#pragma once
// wasm32 wire protocol. Only the central exchange transfers video ownership.
#include "gba_ppu.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace gbarecomp::web {
constexpr uint32_t Magic = 0x47425257, Version = 1, Dirty = 4;
// Ownership handshake for Backend storage during page-side detach. JS may only
// publish SafeToFree after the AudioWorklet stopped and AudioContext closed.
enum DetachState : uint32_t { DetachPending, DetachSafeToFree, DetachUnsafeRetain };
constexpr uint32_t PixelBytes = gba::GbaPpu::kMaxRenderWidth * gba::GbaPpu::kMaxRenderHeight * 3;
constexpr uint32_t AudioSlots = 16, AudioSamples = 2048, CommandSlots = 64;
// Single source of truth: also exported as a named JS descriptor by the backend.
#define GBR_WEB_FIELDS(X) \
 X(magic) X(version) X(bytes) X(generation) X(state) X(middle) \
 X(published) X(replaced) X(filter) X(scale) X(volume) X(audioEnabled) X(fps) \
 X(drawable) X(keys) X(turbo) X(hidden) X(quit) X(fullscreen) \
 X(commandWrite) X(commandRead) X(commandOverflow) X(inputUpdates) X(appliedKeys) \
 X(audioWrite) X(audioRead) X(audioOverflow) X(audioReady) X(hostRate) \
 X(resetRequest) X(resetAck) X(detached) X(audioFill) X(underruns) X(concealed) \
 X(audioConsumed) X(audioPublished) X(audioDspOverflow) X(paused)
struct alignas(64) Control {
#define GBR_WEB_ATOMIC(name) std::atomic<uint32_t> name{0};
 GBR_WEB_FIELDS(GBR_WEB_ATOMIC)
#undef GBR_WEB_ATOMIC
};
struct VideoSlot { uint32_t width, height, stride, seq; uint8_t pixels[PixelBytes]; };
struct AudioSlot { uint32_t count, rate, epoch; int16_t samples[AudioSamples]; };
struct Command { uint32_t seq, kind, arg; };
struct Shared { Control control; VideoSlot video[3]; AudioSlot audio[AudioSlots]; Command commands[CommandSlots]; };
enum State : uint32_t { Starting, Running, Stopping, Failed };
enum CommandKind : uint32_t { Pause=1, Save, Load, Bigger, Smaller, VolumeUp, VolumeDown, Fps, Rewind, SolarUp, SolarDown, SolarLive };
static_assert(sizeof(std::atomic<uint32_t>) == 4 && std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::is_standard_layout_v<Shared> && alignof(Shared) >= 4);
static_assert(PixelBytes == 230400 && offsetof(VideoSlot, pixels) == 16);
static_assert(sizeof(AudioSlot) == 12 + AudioSamples * 2);
static_assert((AudioSlots & (AudioSlots-1)) == 0 && (CommandSlots & (CommandSlots-1)) == 0);
#define GBR_WEB_ASSERT(name) static_assert(offsetof(Control, name) % 4 == 0);
GBR_WEB_FIELDS(GBR_WEB_ASSERT)
#undef GBR_WEB_ASSERT
}
