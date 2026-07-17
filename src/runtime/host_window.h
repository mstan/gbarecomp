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

#pragma once

#include <cstddef>
#include <cstdint>

namespace gbarecomp {

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
    // `linear_filter` selects linear (vs nearest) texture scaling — the
    // launcher's "Linear filtering" toggle; default preserves the historical
    // nearest look.
    bool open(int scale = 3, int base_w = 240, int base_h = 160,
              const char* title = "gbarecomp", const char* screen = nullptr,
              bool linear_filter = false);
    void close();
    bool is_open() const { return open_; }

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
    void load_input_config(const char* dir);

    // Live window/audio controls (hotkey + launcher-driven). All no-ops when
    // the window isn't open or this build has no SDL2.
    void set_fullscreen(bool on);
    bool fullscreen() const;
    void adjust_scale(int delta);       // integer window scale, clamped 1..8
    void set_volume(int pct);           // 0..100, applied to pushed samples
    int  volume() const;
    void set_fps_readout(bool on);      // presents-per-second in the title bar
    bool fps_readout() const;

    // Upload one base_w x base_h RGB888 frame (the dimensions passed to open())
    // and present.
    void present(const uint8_t* rgb888);

    // Push `count` int16_t mono samples (32.768 kHz) into the audio
    // output queue. Backend converts to the host device's format.
    // No-op if audio init failed or this build has no SDL2.
    void push_audio_samples(const int16_t* samples, std::size_t count);

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
        // Level-triggered: true while the fast-forward (Turbo) binding is
        // held (default Tab). Uncaps the frame limiter for as long as it's
        // down.
        bool     fast_forward = false;
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
    };
    Events pump();

private:
    bool open_ = false;
    void* impl_ = nullptr;  // backend-specific opaque
};

}  // namespace gbarecomp
