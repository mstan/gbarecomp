// host_window.h — minimal host window + input surface.
//
// Soft-dependency on SDL2. When the build can find SDL2 the cpp
// uses it; otherwise the same symbols compile as no-op stubs so
// headless builds (CI, BIOS smoke without --window) still link.
//
// The window owns a logical-size streaming texture matching the active GBA
// framebuffer pixel format (RGB888). Expanded views opt into a resizable,
// aspect-correct viewport; the faithful 240x160 path retains the historical
// fixed SDL presentation. pump() drains the OS event queue, returns a quit flag
// and a packed GBA KEYINPUT value (active-low, 1 = released).
//
// Browser builds (GBARECOMP_WEB_HOST) implement the same class in
// host_window_web.cpp without SDL: present() publishes RGB888 into a shared
// triple buffer consumed by page-owned WebGL (packaging/web/host_web.js),
// pump() reads an input snapshot plus a command ring, and audio goes to an
// AudioWorklet ring. Touch, haptics, host overlays and the mobile lifecycle map
// onto browser equivalents (pointer events -> TouchHub, navigator.vibrate, a
// 2D-canvas display list, page visibility). Capabilities the browser lacks
// (gyro, solar keys without bindings, exclusive fullscreen, runtime UI overlay,
// overlay text) are reported absent, never simulated. See
// packaging/web/README.md.

#pragma once

#include <cstddef>
#include <cstdint>

struct RecompRuntimeUi;

namespace gbarecomp {

class HostOverlay;

class HostWindow {
public:
    HostWindow();
    ~HostWindow();

    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    // True if this build was compiled against a real windowing
    // backend. When false, open() always fails.
    static bool is_available();

    // Open a window. `scale` is the integer scale factor applied to
    // the logical surface, whose size is `base_w` x `base_h` (240x160 for the
    // faithful view, wider when view-area expansion is active). Returns false on
    // failure (also when is_available() is false).
    // `screen` is the per-game color model from [video].screen in game.toml
    // (raw|unlit|frontlit|backlit|classic), or nullptr for none. The
    // GBARECOMP_SCREEN env var, when set, overrides it.
    // `linear_filter` selects ordinary linear scaling. `sharp_filter` uses a
    // GPU integer prescale followed by a small fractional linear pass and
    // takes precedence when both are requested.
    bool open(int scale = 3, int base_w = 240, int base_h = 160,
              const char* title = "gbarecomp", const char* screen = nullptr,
              bool linear_filter = false, bool sharp_filter = false,
              bool resize_driven_view = false,
              bool freely_resizable_window = false);
    void close();
    bool is_open() const { return open_; }

    // Resize the logical streaming surface without changing the host window.
    // Used only by the explicit resize-driven view policy; fixed-width callers
    // never invoke it. drawable_size() reports the live client-window extent
    // (and therefore follows drag-resize and borderless desktop fullscreen).
    bool set_surface_size(int base_w, int base_h);
    bool drawable_size(int* width, int* height) const;

    // Load player keybinds + system hotkeys from `dir` (the exe directory):
    //   * keybinds.ini — recomp-ui's generic keybinds format ([player1],
    //     SDL scancode names). Absent file => the built-in defaults below,
    //     which MATCH recomp-ui's defaults so the launcher rebind page and
    //     the game always agree: A=X B=Z L=C R=V Start=Return Select=RShift
    //     + arrow keys.
    //   * config.ini [KeyMap] — hotkey bindings (SDL keycode names with
    //     Ctrl+/Alt+/Shift+ prefixes): Fullscreen, Pause, Turbo,
    //     WindowBigger, WindowSmaller, VolumeUp, VolumeDown, DisplayPerf.
    // Never called => built-in defaults for both. Safe to call when the
    // files don't exist.
    void load_input_config(const char* dir,
                           bool assist_tools_default = true,
                           int fast_forward_multiplier_default = 4);

    // Live window/audio controls (hotkey + launcher-driven). All no-ops when
    // the window isn't open or this build has no SDL2.
    // Tri-state fullscreen mode: 0 windowed, 1 borderless
    // (SDL_WINDOW_FULLSCREEN_DESKTOP), 2 exclusive (SDL_WINDOW_FULLSCREEN).
    // Out-of-range values clamp to 0..2. fullscreen() reports the current mode.
    void set_fullscreen(int mode);
    int  fullscreen() const;
    void adjust_scale(int delta);       // integer window scale, clamped 1..8
    void set_volume(int pct);           // 0..100, applied to pushed samples
    int  volume() const;
    int  window_scale() const;
    void set_linear_filter(bool enabled);
    bool linear_filter() const;
    void set_audio_enabled(bool enabled);
    bool audio_enabled() const;
    void set_resize_driven_view(bool enabled);
#if defined(GBARECOMP_RUNTIME_UI)
    // Attach the capability-only shared model. HostWindow presents it through
    // Dear ImGui's SDL_Renderer2 backend over the existing game renderer.
    void set_runtime_ui(RecompRuntimeUi* ui);
#endif
    // ---- touch (touch_input.h) ---------------------------------------------
    // `policy` = the game consumes touch through RunOptions::input_frame;
    // non-pad touches are then routed to the TouchHub instead of only opening
    // settings. `claims` are the game's TouchClaim bits. `pad_default` is the
    // virtual-pad visibility when config.ini holds no saved choice (-1 =
    // platform default). `emulate_touch` turns desktop mouse input into touch
    // (left = finger, right = two-finger tap, middle = three-finger tap,
    // Backspace = platform Back) for development and validation.
    void configure_touch(bool policy, std::uint32_t claims, int pad_default,
                         bool emulate_touch);
    // Loads/persists the player's pad choice in <dir>/config.ini [Touch].
    void set_touch_config_dir(const char* dir);
    bool touch_pad_visible() const;
    void set_touch_pad_visible(bool visible);
    // Logical view margins inside the surface (for touch/overlay mapping).
    void set_view_margins(std::uint32_t left, std::uint32_t right,
                          std::uint32_t top, std::uint32_t bottom);
    // Game-owned host overlay, drawn each present (null = none).
    void set_host_overlay(void (*overlay)(HostOverlay*));
    // Physical density of the window's display (drawable px per mm).
    float px_per_mm() const;
    // Portrait presentations may anchor the image to the top edge so host
    // chrome below it gets the remaining space (per-present game request).
    void set_presentation_anchor_top(bool anchor_top);
    // Orientation policy for rotating platforms: 0 landscape, 1 portrait,
    // 2 any. Must be set BEFORE open(); ignored on desktop.
    void set_orientation_policy(int policy) { orientation_policy_ = policy; }

    // Mobile lifecycle. pump() reports entering the background once; the
    // runtime then persists state and calls wait_for_foreground(), which
    // blocks (audio paused) until the app returns (true) or quits (false).
    bool wait_for_foreground();

    // Short vibration where the platform supports it (no-op otherwise).
    void haptic_pulse(int duration_ms, float strength);

    void set_fps_readout(bool on);      // presents-per-second in the title bar
    bool fps_readout() const;
    bool assist_tools_enabled() const;
    int  fast_forward_multiplier() const;

    // Upload one base_w x base_h RGB888 frame (the dimensions passed to open())
    // and present.
    void present(const uint8_t* rgb888);

    // Push `count` int16_t mono samples into the audio output queue. The SDL
    // backend configures its resampler for a 65.536 kHz source; the real
    // mixer rate follows SOUNDBIAS (GbaAudio::sample_rate(), 32.768 to
    // 262.144 kHz), so rate-aware callers use push_audio_block on the web.
    // Backend converts to the host device's format.
    // No-op if audio init failed or this build has no SDL2.
    void push_audio_samples(const int16_t* samples, std::size_t count);

#if defined(GBARECOMP_WEB_HOST)
    // Rate-tagged mono PCM (one SOUNDBIAS rate per block, from
    // GbaAudio::drain_sample_block). Never blocks the guest: a full ring is
    // counted as overflow and requests a playback reset.
    void push_audio_block(const int16_t* samples, std::size_t count, uint32_t rate);
    // Starts a new playback epoch (pause, state load, overflow). Samples are
    // not accepted until the AudioWorklet acknowledges the reset.
    void reset_audio();
    // True while the page is hidden; the runner holds the guest at the frame
    // boundary, distinct from the user's own pause toggle.
    bool auto_paused() const;
    void report_paused(bool paused);
#endif

    // Service the native window-system queue without consuming input events.
    // Long guest frames use this to remain responsive between presentations.
    void service_events();

    struct Events {
        bool     quit = false;
        // GBA KEYINPUT layout. Active-low: 1 = released, 0 = pressed.
        // Bits: 0=A 1=B 2=Sel 3=Sta 4=Right 5=Left 6=Up 7=Down 8=R 9=L.
        uint16_t keyinput = 0x03FF;
        // Edge-triggered save-state slot hotkeys. F1..F9 load slot
        // 1..9; Shift+F1..F9 save slot 1..9. 0 = no request this pump.
        // The caller acts on these at the top of the loop (a clean
        // dispatch boundary), never mid-frame.
        int      save_slot = 0;
        int      load_slot = 0;
        // Optional, edge-triggered solar controls from config.ini [KeyMap].
        // They have no built-in bindings; recomp-ui exposes them only for
        // cartridges that declare a solar sensor.
        bool     solar_brighter = false;
        bool     solar_dimmer = false;
        bool     solar_live = false;
        // Level-triggered: true while the fast-forward (Turbo) binding is
        // held (default Tab). Uncaps the frame limiter for as long as it's
        // down.
        bool     fast_forward = false;
        // Repeating global Assist binding. It fires immediately, then every
        // half-second while held so one-second rewind steps move backward at
        // approximately normal speed.
        bool     rewind = false;
        // Edge-triggered system hotkeys (config.ini [KeyMap] bindings; see
        // load_input_config). The caller owns the semantics: fullscreen and
        // window scale route back into this window, pause gates stepping in
        // the run loop, volume adjusts pushed-sample gain, FPS toggles the
        // title-bar readout.
        bool     toggle_fullscreen = false;
        bool     toggle_pause = false;
        bool     window_bigger = false;
        bool     window_smaller = false;
        bool     volume_up = false;
        bool     volume_down = false;
        bool     toggle_fps = false;
        // Horizontal mouse velocity while the left button is held. The runtime
        // maps this host-neutral delta onto cartridge gyroscope sample units.
        int      gyro_delta_x = 0;
        bool     mouse_gyro_active = false;
        // Controller angular velocity around its face-normal axis, in rad/s.
        // A DualSense supplies this through SDL's standard gyro sensor API.
        float    gyro_rate_z = 0.0f;
        // Mobile lifecycle: the OS is moving the app to the background (or
        // terminating it). The runtime must flush saves NOW; see
        // wait_for_foreground().
        bool     enter_background = false;
        bool     terminating = false;
    };
    Events pump();

private:
    bool open_ = false;
    void* impl_ = nullptr;  // backend-specific opaque
    int orientation_policy_ = 0;
};

// Engine-wide haptic hook for game input policies (touch_input.h users).
// Installed by the open HostWindow; a no-op when no window/vibrator exists.
void host_haptic_pulse(int duration_ms, float strength);

// Ask the open window to show the runtime settings menu at its next pump
// (thread-safe). Lets a policy that claims Back/long-press still offer the
// menu from a context of its choosing, e.g. Back on the free overworld.
void host_request_settings_menu();

#if defined(GBARECOMP_WEB_HOST)
// Persistent browser storage: the page (packaging/web/save_store.js) owns
// when /saves is synced to IndexedDB; the runtime only reports that it wrote.
enum class WebStorageWrite : uint32_t { Battery = 1, State = 2 };
// Worker -> page, non-blocking. Integers only (async proxy). Independent of
// HostWindow lifetime: the final battery flush runs after HostWindow::close().
void web_notify_storage_write(WebStorageWrite kind, bool ok);
#endif

}  // namespace gbarecomp
