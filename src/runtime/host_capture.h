#pragma once
// Unattended presented-frame and audio capture (--capture-out and friends).
//
// The normal window presenter runs in a never-shown window of exactly
// --capture-size, so adaptive widescreen, filtering and scaling behave as in a
// visible window. Each presented frame is read back and written as
// frame_NNNNNN.png|ppm; the mixed output is written as a 48 kHz stereo WAV.
// Pacing, the host audio device, host overlays and live input are off. Input
// comes from --input-script ("<frame> BUTTONS", '-' releases).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gbarecomp::capture {

struct InputEvent {
    uint64_t frame;
    uint16_t keyinput;  // active-low GBA KEYINPUT
};

// Consumes argv[*i] (and its value) when it is a capture option.
bool parse_arg(int argc, char** argv, int* i);
bool requested();
bool failed();
void window_size(int* w, int* h);                // 0x0 when --capture-size was not given
bool load_script(std::vector<InputEvent>* out);  // false on a bad script

// PNG sink supplied by the runtime (RGB24, top-down).
using PngWriter = bool (*)(const std::string& path, const uint8_t* rgb, uint32_t w, uint32_t h);
void set_png_writer(PngWriter writer);

// Called by the window presenter for every presented frame (RGB24 top-down).
void on_frame(const uint8_t* rgb, int w, int h);
// Mixed stereo S16 at `rate` Hz (the guest block rate); resampled to 48 kHz.
void on_audio(const int16_t* stereo, std::size_t frames, uint32_t rate);

bool want_readback();   // the next present is inside the range
bool should_exit();     // range finished
uint64_t frame_index(); // presents so far
void finish();          // flush, patch WAV header, print summary

}  // namespace gbarecomp::capture
