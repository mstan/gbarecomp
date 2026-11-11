#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "host_window.h"
#include "presentation_settings.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

static std::vector<unsigned char> captured;
static int width, height, failures;
extern "C" void __real_SDL_RenderPresent(SDL_Renderer*);
extern "C" void __wrap_SDL_RenderPresent(SDL_Renderer* renderer) {
    SDL_GetRendererOutputSize(renderer, &width, &height);
    int lw, lh;
    SDL_RenderGetLogicalSize(renderer, &lw, &lh);
    SDL_RenderSetLogicalSize(renderer, 0, 0);
    SDL_RenderSetViewport(renderer, nullptr);
    captured.resize(width * height * 4);
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32,
                             captured.data(), width * 4)) std::abort();
    SDL_RenderSetLogicalSize(renderer, lw, lh);
    __real_SDL_RenderPresent(renderer);
}
static void check(bool yes, const char* what) {
    if (!yes) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
static unsigned red(int x, int y) { return captured[(y * width + x) * 4]; }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path fixture(argv[1]);
    std::filesystem::create_directories(fixture);
    const auto config = fixture / "config.ini";
    { std::ofstream f(config); f << "[Launcher]\nvolume = 73\nplayer_guid = retained\n[KeyMap]\nScreenshot = 40\n"; }
    SDL_SetMainReady();
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_setenv("GBARECOMP_WINDOW_HIDDEN", "1", 1);
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    {
        gbarecomp::HostWindow window;
        if (!window.open(3, 240, 160, "Presentation fixture", "raw")) return 3;
        window.load_input_config(fixture.string().c_str());
        auto* native = SDL_GetWindowFromID(1);
        if (!native) return 4;
        SDL_SetWindowSize(native, 1000, 700);
        window.pump();
        std::vector<unsigned char> rgb(240 * 160 * 3, 240);
        window.present(rgb.data());
        check(width == 1000 && height == 700, "noninteger drawable resized");
        check(red(10, 350) == 240, "responsive fit uses fractional width");
        window.set_integer_scale(true);
        window.present(rgb.data());
        const auto integer = captured;
        check(red(10, 350) == 7 && red(20, 30) == 240 && red(979, 669) == 240,
              "integer fit centers whole 960x640 pixels");
        window.set_scanlines(true, 50);
        window.present(rgb.data());
        check(red(500, 30) == 240 && red(500, 32) >= 119 && red(500, 32) <= 121,
              "scanline strength darkens only lower half of a logical line");
        check(red(10, 350) == 7, "scanlines preserve existing presentation bars");
        window.set_scanlines(false, 50);
        window.present(rgb.data());
        check(captured == integer, "disabling effect restores exact prior image");
        window.set_scanlines(true, 50);
        SDL_SetWindowSize(native, 240, 160); window.pump(); window.present(rgb.data());
        check(red(100, 20) == 240 && red(100, 21) == 240, "1x avoids aliasing bands");
        window.set_scanlines(true, 999);
        check(window.scanline_strength() == 100, "effect strength clamps invalid input");
        window.set_scanlines(false, 35);
        check(window.set_surface_size(427, 240), "adaptive-sized surface recreated");
        window.set_resize_driven_view(true);
        SDL_SetWindowSize(native, 1280, 720); window.pump();
        rgb.assign(427 * 240 * 3, 240);
        window.present(rgb.data());
        check(red(100, 360) == 7 && red(213, 120) == 240 && red(1066, 599) == 240,
              "integer fit handles nonnative logical geometry");
        window.set_integer_scale(false); window.present(rgb.data());
        check(red(100, 360) == 240, "adaptive fit fills drawable after integer toggle off");
        window.set_linear_filter(true);
        window.set_integer_scale(true);
        for (int y = 0; y < 240; ++y) for (int x = 0; x < 427; ++x)
            rgb[(y * 427 + x) * 3] = x % 2 ? 40 : 240;
        window.present(rgb.data());
        check(red(213, 300) == 240 && red(214, 300) == 240 &&
              red(215, 300) == 40 && red(216, 300) == 40,
              "pixel-perfect fit retains nearest pixels with smooth preference saved");
        window.set_scanlines(true, 100);
    }
    std::ifstream f(config);
    const std::string saved{std::istreambuf_iterator<char>(f), {}};
    check(saved.find("integer_scale = 1") != std::string::npos &&
          saved.find("scanlines = 1") != std::string::npos &&
          saved.find("scanline_strength = 100") != std::string::npos,
          "presentation settings persist");
    check(saved.find("volume = 73") != std::string::npos &&
          saved.find("player_guid = retained") != std::string::npos &&
          saved.find("Screenshot = 40") != std::string::npos, "foreign settings retained");
    SDL_Quit();
    std::printf("display contract: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
