// host_window.h — minimal host window + input surface.
//
// Soft-dependency on SDL2. When the build can find SDL2 the cpp
// uses it; otherwise the same symbols compile as no-op stubs so
// headless builds (CI, BIOS smoke without --window) still link.
//
// The window owns a 240x160 streaming texture matching the GBA
// framebuffer pixel format (RGB888). present() uploads a frame and
// flips. pump() drains the OS event queue, returns a quit flag and
// a packed GBA KEYINPUT value (active-low, 1 = released).

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
    // the 240x160 logical surface. Returns false on failure (also
    // when is_available() is false).
    bool open(int scale = 3, const char* title = "gbarecomp");
    void close();
    bool is_open() const { return open_; }

    // Upload a 240x160 RGB888 frame and present.
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
        // Level-triggered: true while the fast-forward key (Tab) is
        // held. Uncaps the frame limiter for as long as it's down.
        bool     fast_forward = false;
    };
    Events pump();

private:
    bool open_ = false;
    void* impl_ = nullptr;  // backend-specific opaque
};

}  // namespace gbarecomp
