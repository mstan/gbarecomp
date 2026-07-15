// host_window.cpp — SDL2-backed window. Stubs out when SDL2 isn't found.

#include "host_window.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "color_lut.h"
#include "presentation_layout.h"

#if defined(GBARECOMP_HAVE_SDL2)

#include <SDL.h>

// Shared ecosystem clock-domain bridge (callback-driven DRC). Replaces the
// SDL_QueueAudio push path, which silence-filled on queue underrun (~3.4/s
// measured on Minish Cap) and hard-flushed on overflow — the same output-side
// crackle fixed on NES. IMPL is defined in exactly this one translation unit.
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"

namespace gbarecomp {

namespace {

struct Backend {
    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture*  texture  = nullptr;
    SDL_AudioDeviceID audio_dev = 0;
    // Callback-driven clock-domain bridge (replaces SDL queue push).
    rab_bridge    bridge{};
    bool          bridge_ready = false;
    SDL_mutex*    audio_mtx = nullptr;
    // Present-time screen-color simulation. Built once from
    // GBARECOMP_SCREEN; default Raw = exact passthrough (no copy, no
    // grading), so default behavior is byte-identical to upstream.
    std::unique_ptr<runtime::ColorLut> color_lut;
    std::vector<uint8_t> graded_fb;  // scratch RGB888 (base_w*base_h*3)
    int base_w = 240;   // logical surface width  (240 faithful, wider if expanded)
    int base_h = 160;   // logical surface height (160; vertical expansion deferred)
    bool expanded_view = false;  // native games retain the historical SDL path
};

// Resolve the screen model: the per-game [video].screen from game.toml
// (`toml_screen`) is the default; GBARECOMP_SCREEN overrides it at launch.
// Unset/unrecognized → Raw (passthrough). Tokens: raw|unlit|frontlit|
// backlit|classic.
runtime::ColorSettings resolve_color_settings(const char* toml_screen) {
    runtime::ColorSettings s;
    runtime::ScreenKind k;
    if (toml_screen && runtime::screen_kind_from_name(toml_screen, k)) s.screen = k;
    if (const char* env = std::getenv("GBARECOMP_SCREEN")) {
        if (runtime::screen_kind_from_name(env, k)) s.screen = k;
    }
    return s;
}

// Map an SDL_Scancode to a GBA KEYINPUT bit index, or -1 if unmapped.
// Default GBA layout:
//   bit 0 = A      mapped to Z
//   bit 1 = B      mapped to X
//   bit 2 = Select mapped to RShift
//   bit 3 = Start  mapped to Return
//   bit 4 = Right  Arrow
//   bit 5 = Left   Arrow
//   bit 6 = Up     Arrow
//   bit 7 = Down   Arrow
//   bit 8 = R btn  mapped to S
//   bit 9 = L btn  mapped to A
int scancode_to_gba_bit(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_Z:      return 0;
        case SDL_SCANCODE_X:      return 1;
        case SDL_SCANCODE_RSHIFT: return 2;
        case SDL_SCANCODE_RETURN: return 3;
        case SDL_SCANCODE_RIGHT:  return 4;
        case SDL_SCANCODE_LEFT:   return 5;
        case SDL_SCANCODE_UP:     return 6;
        case SDL_SCANCODE_DOWN:   return 7;
        case SDL_SCANCODE_S:      return 8;
        case SDL_SCANCODE_A:      return 9;
        default:                  return -1;
    }
}

// SDL audio pull callback: render exactly `len` bytes of device-rate mono S16
// from the bridge ring. The bridge emits faded silence (not raw zeros) before
// prime / on underrun, so a momentarily-starved producer no longer clicks.
void gba_audio_callback(void* userdata, Uint8* stream, int len) {
    auto* b = static_cast<Backend*>(userdata);
    int frames = len / static_cast<int>(sizeof(int16_t)); // mono
    if (b && b->bridge_ready) {
        SDL_LockMutex(b->audio_mtx);
        rab_pull(&b->bridge, reinterpret_cast<int16_t*>(stream), frames);
        SDL_UnlockMutex(b->audio_mtx);
    } else {
        SDL_memset(stream, 0, len);
    }
}

}  // namespace

HostWindow::HostWindow() = default;

HostWindow::~HostWindow() {
    close();
}

bool HostWindow::is_available() { return true; }

bool HostWindow::open(int scale, int base_w, int base_h, const char* title,
                      const char* screen) {
    if (open_) return true;
    if (scale < 1) scale = 1;
    if (base_w < 1) base_w = 240;
    if (base_h < 1) base_h = 160;

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "host_window: SDL_InitSubSystem(VIDEO) failed: %s\n",
                         SDL_GetError());
            return false;
        }
    }
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        // Non-fatal if audio fails — keep video working.
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            std::fprintf(stderr, "host_window: SDL_InitSubSystem(AUDIO) failed: %s\n",
                         SDL_GetError());
        }
    }

    auto* b = new Backend{};
    b->base_w = base_w;
    b->base_h = base_h;
    b->expanded_view = base_w != 240 || base_h != 160;
    const int win_w = base_w * scale;
    const int win_h = base_h * scale;
    const Uint32 window_flags = SDL_WINDOW_SHOWN |
        (b->expanded_view ? SDL_WINDOW_RESIZABLE : 0u);
    b->window = SDL_CreateWindow(title ? title : "gbarecomp",
                                 SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED,
                                 win_w, win_h,
                                 window_flags);
    if (!b->window) {
        std::fprintf(stderr, "host_window: SDL_CreateWindow failed: %s\n",
                     SDL_GetError());
        delete b;
        return false;
    }
    // No PRESENTVSYNC: emulation speed is governed by FramePacer at the
    // GBA's 59.7275 Hz, not the host monitor's refresh rate. Coupling to
    // vsync ran the game at monitor speed (MC-HP-004). present() must
    // not block on the display.
    b->renderer = SDL_CreateRenderer(b->window, -1, SDL_RENDERER_ACCELERATED);
    if (!b->renderer) {
        // Fall back to software renderer if accelerated path is
        // unavailable (headless Windows, RDP, etc.).
        b->renderer = SDL_CreateRenderer(b->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!b->renderer) {
        std::fprintf(stderr, "host_window: SDL_CreateRenderer failed: %s\n",
                     SDL_GetError());
        SDL_DestroyWindow(b->window);
        delete b;
        return false;
    }
    if (b->expanded_view) {
        // The destination viewport is computed explicitly in present() so
        // resizing maximally fills the drawable at the selected widescreen
        // aspect. Exact multiples retain integer scale and nearest filtering.
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
        SDL_SetRenderDrawColor(b->renderer, 0, 0, 0, 255);
    } else {
        // Non-opting games retain the historical fixed 240x160 SDL logical
        // renderer, including its window flags and copy path.
        SDL_RenderSetLogicalSize(b->renderer, base_w, base_h);
    }

    b->texture = SDL_CreateTexture(b->renderer,
                                   SDL_PIXELFORMAT_RGB24,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   base_w, base_h);
    if (!b->texture) {
        std::fprintf(stderr, "host_window: SDL_CreateTexture failed: %s\n",
                     SDL_GetError());
        SDL_DestroyRenderer(b->renderer);
        SDL_DestroyWindow(b->window);
        delete b;
        return false;
    }

    // Open the audio device at 65536 Hz mono 16-bit signed. The
    // running BIOS sets SOUNDBIAS resolution=1 which raises the
    // mixer's effective sample rate from 32768 to 65536; opening
    // the device at 32768 (the power-on default) caused SDL to play
    // back at half speed, hit the 250 ms queue cap, and flush —
    // audible as muffled / watery chime artifacts. SDL2 resamples
    // internally if the host hardware doesn't natively support
    // 65536, so this rate is portable. Failure is non-fatal —
    // silent video still works.
    SDL_AudioSpec want{};
    want.freq     = 65536;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 1024;  // ~15 ms callback quantum at 65 kHz
    want.callback = gba_audio_callback;  // pull mode (bridge-fed)
    want.userdata = b;
    SDL_AudioSpec got{};
    // Allow the device to pick its native rate; the bridge resamples the GBA
    // mixer rate (65536) to whatever the device opened at.
    b->audio_dev = SDL_OpenAudioDevice(nullptr, /*iscapture=*/0,
                                       &want, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (b->audio_dev != 0) {
        b->audio_mtx = SDL_CreateMutex();
        rab_config cfg;
        rab_config_defaults(&cfg);
        cfg.channels    = 1;
        cfg.source_rate = 65536.0;                 // engine's standardized GBA mixer rate
        cfg.host_rate   = static_cast<double>(got.freq);
        cfg.target_ms   = 60.0;                     // steady cushion (matches NES)
        cfg.preroll_ms  = 250.0;                    // boot pre-roll: hide the cold-start
                                                    // recomp warm-up hitch (drains to target)
        if (rab_init(&b->bridge, &cfg) == 0) b->bridge_ready = true;
        SDL_PauseAudioDevice(b->audio_dev, 0);      // start the callback
    }

    b->color_lut = std::make_unique<runtime::ColorLut>(resolve_color_settings(screen));
    if (!b->color_lut->is_passthrough())
        b->graded_fb.resize(static_cast<std::size_t>(base_w) * base_h * 3);

    impl_ = b;
    open_ = true;
    return true;
}

// Game-thread codegen time (overlay_loader.cpp) — forward-declared to avoid a
// heavy include. ~0 during async play; >0 means the game thread is compiling.
// (We are already inside namespace gbarecomp, so declare it unqualified.)
uint64_t overlay_game_thread_compile_ns();

void HostWindow::push_audio_samples(const int16_t* samples, std::size_t count) {
    if (!open_ || !impl_ || !samples || count == 0) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->audio_dev == 0 || !b->bridge_ready) return;

    // Producer: append mono frames into the bridge ring. The SDL callback
    // (gba_audio_callback) drains it at the device rate with band-limited
    // resampling + a P-only fill servo — no queue underrun, no hard flush.
    SDL_LockMutex(b->audio_mtx);
    rab_push(&b->bridge, samples, static_cast<int>(count)); // mono: count == frames
    SDL_UnlockMutex(b->audio_mtx);

    // ── NES-mode crackle probe (measure step) ──────────────────────────
    // GBARECOMP_AUDIO_PROBE=1 reports the BRIDGE's underrun/overflow counters
    // (the post-fix equivalent of SDL queue underruns) so a before/after is
    // directly comparable. Expect ~0 underruns once primed.
    static int s_probe = -1;
    if (s_probe < 0) { const char* e = std::getenv("GBARECOMP_AUDIO_PROBE"); s_probe = (e && *e && *e != '0') ? 1 : 0; }
    if (s_probe) {
        static unsigned long long s_pushes = 0, s_samples = 0;
        s_pushes++; s_samples += count;
        if ((s_pushes % 120ULL) == 0ULL) {
            rab_stats st; rab_get_stats(&b->bridge, &st);
            double secs = static_cast<double>(s_samples) / 65536.0;
            double stretch_ms = st.stretch_frames * 1000.0
                              / static_cast<double>(b->bridge.cfg.host_rate);
            // Game-thread codegen time: cumulative + this-window delta. The delta
            // is the smoking gun — async play holds it at 0; sync play grows it.
            static unsigned long long s_prev_cc_ns = 0;
            unsigned long long cc_ns = overlay_game_thread_compile_ns();
            double gt_ms      = cc_ns / 1e6;
            double gt_dms     = (cc_ns - s_prev_cc_ns) / 1e6;
            s_prev_cc_ns = cc_ns;
            std::fprintf(stderr,
                "[gba-audio-probe] pushes=%llu audio=%.1fs bridge_underrun=%llu(%.2f/s) "
                "stretch=%.0fms(ev=%llu) overflow_drops=%llu fill_ms=%.1f corr=%+.3f%% "
                "gt_compile=%.1fms(+%.1fms)\n",
                s_pushes, secs, (unsigned long long)st.underrun_events,
                secs > 0 ? st.underrun_events / secs : 0.0,
                stretch_ms, (unsigned long long)st.stretch_events,
                (unsigned long long)st.overflow_drops, rab_fill_ms(&b->bridge),
                st.last_correction * 100.0, gt_ms, gt_dms);
            std::fflush(stderr);
        }
    }
}

void HostWindow::close() {
    if (!impl_) { open_ = false; return; }
    auto* b = static_cast<Backend*>(impl_);
    if (b->audio_dev) SDL_CloseAudioDevice(b->audio_dev);  // stops the callback first
    if (b->bridge_ready) rab_free(&b->bridge);
    if (b->audio_mtx) SDL_DestroyMutex(b->audio_mtx);
    if (b->texture)   SDL_DestroyTexture(b->texture);
    if (b->renderer)  SDL_DestroyRenderer(b->renderer);
    if (b->window)    SDL_DestroyWindow(b->window);
    delete b;
    impl_ = nullptr;
    open_ = false;
}

void HostWindow::present(const uint8_t* rgb888) {
    if (!open_ || !impl_ || !rgb888) return;
    auto* b = static_cast<Backend*>(impl_);
    // Present-time color grading (opt-in). Raw is passthrough: the raw
    // PPU frame is uploaded untouched, so verify/frame-hash are unaffected.
    if (b->color_lut && !b->color_lut->is_passthrough() &&
        b->graded_fb.size() == static_cast<std::size_t>(b->base_w) * b->base_h * 3u) {
        b->color_lut->map_rgb888(rgb888, b->graded_fb.data(), b->base_w, b->base_h);
        rgb888 = b->graded_fb.data();
    }
    SDL_UpdateTexture(b->texture, nullptr, rgb888, b->base_w * 3);
    SDL_RenderClear(b->renderer);
    if (!b->expanded_view) {
        SDL_RenderCopy(b->renderer, b->texture, nullptr, nullptr);
    } else {
        int drawable_w = 0;
        int drawable_h = 0;
        if (SDL_GetRendererOutputSize(b->renderer, &drawable_w, &drawable_h) != 0) {
            SDL_GetWindowSize(b->window, &drawable_w, &drawable_h);
        }
        const PresentationLayout layout = compute_presentation_layout(
            drawable_w, drawable_h, b->base_w, b->base_h);
        if (layout.width > 0 && layout.height > 0) {
            const SDL_Rect destination = {
                layout.x, layout.y, layout.width, layout.height};
            SDL_RenderCopy(b->renderer, b->texture, nullptr, &destination);
        }
    }
    SDL_RenderPresent(b->renderer);
}

HostWindow::Events HostWindow::pump() {
    Events ev{};
    if (!open_) { ev.quit = true; return ev; }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            ev.quit = true;
        } else if (e.type == SDL_WINDOWEVENT &&
                   e.window.event == SDL_WINDOWEVENT_CLOSE) {
            ev.quit = true;
        } else if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
            // Edge-triggered hotkeys (ignore key-repeat). F1..F9 are
            // save-state slots: plain = load, Shift = save. SDL's F1..F12
            // keycodes are contiguous, so slot = sym - F1 + 1.
            SDL_Keycode sym = e.key.keysym.sym;
            if (sym == SDLK_ESCAPE) {
                ev.quit = true;
            } else if (sym >= SDLK_F1 && sym <= SDLK_F9) {
                int slot = static_cast<int>(sym - SDLK_F1) + 1;
                if (e.key.keysym.mod & KMOD_SHIFT) ev.save_slot = slot;
                else                               ev.load_slot = slot;
            }
        }
    }

    // Build the GBA KEYINPUT value from current keyboard state.
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    uint16_t keys = 0x03FFu;  // all released
    for (int sc = 0; sc < SDL_NUM_SCANCODES; ++sc) {
        if (!ks[sc]) continue;
        int bit = scancode_to_gba_bit(static_cast<SDL_Scancode>(sc));
        if (bit >= 0) keys &= static_cast<uint16_t>(~(1u << bit));
    }
    ev.keyinput = keys;
    ev.fast_forward = ks[SDL_SCANCODE_TAB] != 0;  // hold Tab to uncap
    return ev;
}

}  // namespace gbarecomp

#else  // !GBARECOMP_HAVE_SDL2 — stub backend

namespace gbarecomp {

HostWindow::HostWindow()  = default;
HostWindow::~HostWindow() = default;

bool HostWindow::is_available() { return false; }

bool HostWindow::open(int /*scale*/, int /*base_w*/, int /*base_h*/,
                      const char* /*title*/, const char* /*screen*/) {
    std::fprintf(stderr,
                 "host_window: built without SDL2; --window unavailable\n");
    return false;
}

void HostWindow::close() { open_ = false; }

void HostWindow::present(const uint8_t* /*rgb888*/) {}

void HostWindow::push_audio_samples(const int16_t* /*samples*/,
                                    std::size_t /*count*/) {}

HostWindow::Events HostWindow::pump() {
    Events ev{};
    ev.quit = true;
    return ev;
}

}  // namespace gbarecomp

#endif  // GBARECOMP_HAVE_SDL2
