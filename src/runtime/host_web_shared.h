#pragma once
// wasm32 wire protocol. Only the central exchange transfers video ownership.
#include "gba_ppu.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace gbarecomp::web {
// Version 2: touch ring, per-frame host overlay display list, page-published
// presentation geometry (layout, insets, density) and pad state.
constexpr uint32_t Magic = 0x47425257, Version = 2, Dirty = 4;
// Ownership handshake for Backend storage during page-side detach. JS may only
// publish SafeToFree after the AudioWorklet stopped and AudioContext closed.
enum DetachState : uint32_t { DetachPending, DetachSafeToFree, DetachUnsafeRetain };
constexpr uint32_t PixelBytes = gba::GbaPpu::kMaxRenderWidth * gba::GbaPpu::kMaxRenderHeight * 3;
constexpr uint32_t AudioSlots = 16, AudioSamples = 2048, CommandSlots = 64;
constexpr uint32_t TouchSlots = 128, OverlayCmds = 1024;
// Single source of truth: also exported as a named JS descriptor by the backend.
//   touchMode      worker -> page: TouchMode bits (configure_touch)
//   touchControls  page -> worker: the page offers a virtual pad (touch device
//                  or touch emulation)
//   padVisible     page -> worker: the page's virtual pad is shown
//   viewXY/viewWH  page -> worker: destination rectangle of the last draw,
//                  drawable pixels, x|y<<16 and w|h<<16
//   insetsLT/RB    page -> worker: safe-area insets inside the canvas, drawable
//                  pixels, left|top<<16 and right|bottom<<16
//   dprMilli       page -> worker: devicePixelRatio * 1000
//   anchorTop      worker -> page: portrait top anchoring requested
#define GBR_WEB_FIELDS(X) \
 X(magic) X(version) X(bytes) X(generation) X(state) X(middle) \
 X(published) X(replaced) X(filter) X(scale) X(volume) X(audioEnabled) X(fps) \
 X(drawable) X(keys) X(turbo) X(hidden) X(quit) X(fullscreen) \
 X(commandWrite) X(commandRead) X(commandOverflow) X(inputUpdates) X(appliedKeys) \
 X(audioWrite) X(audioRead) X(audioOverflow) X(audioReady) X(hostRate) \
 X(resetRequest) X(resetAck) X(detached) X(audioFill) X(underruns) X(concealed) \
 X(audioConsumed) X(audioPublished) X(audioDspOverflow) X(paused) \
 X(touchWrite) X(touchRead) X(touchOverflow) X(touchMode) X(touchControls) \
 X(padVisible) X(viewXY) X(viewWH) X(insetsLT) X(insetsRB) X(dprMilli) X(anchorTop)
struct alignas(64) Control {
#define GBR_WEB_ATOMIC(name) std::atomic<uint32_t> name{0};
 GBR_WEB_FIELDS(GBR_WEB_ATOMIC)
#undef GBR_WEB_ATOMIC
};
// Host overlay primitive, already in drawable pixels. Recorded by the worker
// into the video slot it is about to publish, so the display list travels with
// its frame through the triple-buffer exchange and never tears against it.
enum OverlayKind : uint32_t {
 OverlayLine = 1,        // x0 y0 x1 y1 thickness
 OverlayFillRect,        // x y w h rounding
 OverlayStrokeRect,      // x y w h rounding thickness
 OverlayFillCircle,      // cx cy r
 OverlayStrokeCircle,    // cx cy r thickness
 OverlayArc,             // cx cy r thickness start_turns end_turns
};
struct OverlayCmd { uint32_t kind, rgba; float v[6]; };
struct VideoSlot {
 uint32_t width, height, stride, seq;
 uint8_t pixels[PixelBytes];
 uint32_t overlayCount, overlayDropped;
 OverlayCmd overlay[OverlayCmds];
};
struct AudioSlot { uint32_t count, rate, epoch; int16_t samples[AudioSamples]; };
struct Command { uint32_t seq, kind, arg; };
// Page -> worker pointer events (drawable pixels). `kind` is a TouchKind, with
// TouchFromMouse set when desktop touch emulation produced it.
struct TouchSlot { uint32_t kind; int32_t pointer; float x, y; };
struct Shared {
 Control control; VideoSlot video[3]; AudioSlot audio[AudioSlots];
 Command commands[CommandSlots]; TouchSlot touches[TouchSlots];
};
enum State : uint32_t { Starting, Running, Stopping, Failed };
enum CommandKind : uint32_t { Pause=1, Save, Load, Bigger, Smaller, VolumeUp, VolumeDown, Fps, Rewind, SolarUp, SolarDown, SolarLive };
enum TouchKind : uint32_t {
 TouchDown = 0, TouchMove = 1, TouchUp = 2, TouchCancel = 3,
 TouchTwoFingerTap = 16, TouchThreeFingerTap = 17, TouchBack = 18,
 TouchFromMouse = 0x100,
};
enum TouchMode : uint32_t { TouchModePolicy = 1, TouchModeEmulate = 2 };
static_assert(sizeof(std::atomic<uint32_t>) == 4 && std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::is_standard_layout_v<Shared> && alignof(Shared) >= 4);
// The largest logical view (portrait/anchored and 32:9 mod views included) is
// always publishable; the page validates every frame against maxWidth/maxHeight.
static_assert(PixelBytes % 4 == 0 && offsetof(VideoSlot, pixels) == 16);
static_assert(offsetof(VideoSlot, overlayCount) % 4 == 0 && sizeof(OverlayCmd) == 32);
static_assert(sizeof(AudioSlot) == 12 + AudioSamples * 2);
static_assert(sizeof(TouchSlot) == 16 && offsetof(Shared, touches) % 4 == 0);
static_assert((AudioSlots & (AudioSlots-1)) == 0 && (CommandSlots & (CommandSlots-1)) == 0);
static_assert((TouchSlots & (TouchSlots-1)) == 0);
#define GBR_WEB_ASSERT(name) static_assert(offsetof(Control, name) % 4 == 0);
GBR_WEB_FIELDS(GBR_WEB_ASSERT)
#undef GBR_WEB_ASSERT
}
