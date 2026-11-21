#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "touch_controls_ui.h"
#include "touch_input.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

// Reuse the existing FPS-overlay fixture's pre-present readback seam. Reading
// after Present is undefined, and a logical viewport can clip a whole-window
// read. This Linux-only wrapper leaves other supported test builds portable.
#if defined(GBARECOMP_TOUCH_CAPTURE)
static bool capture_next_present = false;
static std::vector<unsigned char> capture_rgba;
static int capture_width = 0, capture_height = 0;
static float presented_scale_x = 0, presented_scale_y = 0;
extern "C" void __real_SDL_RenderPresent(SDL_Renderer*);
extern "C" void __wrap_SDL_RenderPresent(SDL_Renderer* renderer) {
    SDL_RenderGetScale(renderer, &presented_scale_x, &presented_scale_y);
    if (capture_next_present) {
        int lw = 0, lh = 0; float sx = 1, sy = 1; SDL_Rect viewport{};
        SDL_GetRendererOutputSize(renderer, &capture_width, &capture_height);
        SDL_RenderGetLogicalSize(renderer, &lw, &lh);
        SDL_RenderGetScale(renderer, &sx, &sy); SDL_RenderSetScale(renderer, 1, 1);
        SDL_RenderGetViewport(renderer, &viewport);
        SDL_RenderSetLogicalSize(renderer, 0, 0); SDL_RenderSetViewport(renderer, nullptr);
        capture_rgba.resize(std::size_t(capture_width) * capture_height * 4);
        if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32,
                                 capture_rgba.data(), capture_width * 4) != 0) {
            std::fprintf(stderr, "Host readback: %s\n", SDL_GetError());
            capture_rgba.clear();
        }
        SDL_RenderSetLogicalSize(renderer, lw, lh); SDL_RenderSetScale(renderer, 1, 1);
        SDL_RenderSetViewport(renderer, &viewport);
        float unit_x = 1, unit_y = 1; SDL_RenderGetScale(renderer, &unit_x, &unit_y);
        SDL_RenderSetScale(renderer, sx / unit_x, sy / unit_y);
        capture_next_present = false;
    }
    __real_SDL_RenderPresent(renderer);
}
#endif

#if !SDL_VERSION_ATLEAST(2, 24, 0)
int main() { return 77; }
#else
static int failures = 0;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
int main(int argc, char** argv) {
    const auto dir = std::filesystem::path(argc > 1 ? argv[1] : "touch-contract-fixture");
    std::filesystem::create_directories(dir);
    { std::ofstream file(dir / "config.ini"); file << "[Owner]\nKeep=73\n"; }
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_setenv("GBARECOMP_TOUCH_DPI", "96", 1);
    SDL_setenv("GBARECOMP_TOUCH_CONTROLS", "0", 1);
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetMainReady(); check(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0, "SDL init");
    {
        gbarecomp::HostWindow desktop; check(desktop.open(3), "desktop window");
        int value = 1;
        check(!desktop.touch_controls_available() &&
              !gbarecomp::touch_controls_ui_get(&desktop, &value) &&
              !gbarecomp::touch_controls_ui_set(&desktop, 1),
              "unsupported desktop has no active touch setting");
    }
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX; desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.vendor_id = 0xfeed; desc.product_id = 0xbeef; desc.name = "Touch contract controller";
    desc.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1u;
    desc.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1u;
    const int device = SDL_JoystickAttachVirtualEx(&desc);
    SDL_Joystick* pad = device >= 0 ? SDL_JoystickOpen(device) : nullptr;
    check(pad != nullptr, "native virtual controller"); if (!pad) return 1;
    SDL_setenv("GBARECOMP_TOUCH_CONTROLS", "1", 1);
    {
        gbarecomp::HostWindow touch; check(touch.open(3, 240, 160, "Touch controls contract"), "touch-capable native window");
        touch.configure_touch(false, 0, 1, true);
        touch.set_touch_config_dir(dir.string().c_str());
        check(touch.touch_controls_available() && touch.touch_pad_visible(), "fresh visible default");
        const auto item = gbarecomp::touch_controls_ui_item();
        check(item.type == RECOMP_RUNTIME_UI_BOOL && !std::strcmp(item.label, "Touchscreen controls"),
              "friendly visible boolean Settings item");
        RecompRuntimeUiConfig config{};
        config.title = "Touch settings contract"; config.items = &item; config.item_count = 1;
        config.callbacks.context = &touch;
        config.callbacks.get_value = [](void* c, const RecompRuntimeUiItem*, int* v) {
            return gbarecomp::touch_controls_ui_get(static_cast<gbarecomp::HostWindow*>(c), v);
        };
        config.callbacks.set_value = [](void* c, const RecompRuntimeUiItem*, int v) {
            return gbarecomp::touch_controls_ui_set(static_cast<gbarecomp::HostWindow*>(c), v);
        };
        auto* ui = recomp_runtime_ui_create(&config); check(ui != nullptr, "real shared menu model");
        if (!ui) return 1;
        touch.set_runtime_ui(ui);
        recomp_runtime_ui_open(ui);
        recomp_runtime_ui_handle_input(ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
        recomp_runtime_ui_handle_input(ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
        int value = 1;
        check(!touch.touch_pad_visible() && gbarecomp::touch_controls_ui_get(&touch, &value) && !value,
              "actual Settings activation hides the pad through the shared provider");
        recomp_runtime_ui_handle_input(ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
        check(touch.touch_pad_visible(), "Settings activation restores the pad");
        recomp_runtime_ui_close(ui);
        std::vector<std::uint8_t> rgb(240 * 160 * 3); touch.present(rgb.data());
#if defined(GBARECOMP_TOUCH_CAPTURE)
        check(presented_scale_x == 3.0f && presented_scale_y == 3.0f,
              "overlay preserves the native three-times presentation scale");
        // Stop repeating on a scale failure, rather than letting the old
        // exponential transform exhaust memory in SDL's software renderer.
        for (int frame = 0; frame < 12 && presented_scale_x == 3.0f && presented_scale_y == 3.0f; ++frame)
            touch.present(rgb.data());
        check(presented_scale_x == 3.0f && presented_scale_y == 3.0f,
              "repeated touch overlays keep the effective scale stable");
#endif
        auto finger = [&](Uint32 type, SDL_FingerID id, float x, float y) {
            SDL_Event event{}; event.type = type; event.tfinger.touchId = 55;
            event.tfinger.fingerId = id; event.tfinger.x = x; event.tfinger.y = y;
            event.tfinger.pressure = type == SDL_FINGERUP ? 0.0f : 1.0f;
            check(SDL_PushEvent(&event) == 1, "inject native finger event");
            return touch.pump();
        };
        check((finger(SDL_FINGERDOWN, 1, 0.945f, 0.785f).keyinput & 1u) == 0,
              "visible touch A reaches actual game input");
        SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP, 1);
        SDL_JoystickUpdate(); touch.pump();
        check(gbarecomp::touch_controls_ui_set(&touch, 0), "hide through shared provider");
        auto held = touch.pump();
        check((held.keyinput & 1u) && !(held.keyinput & 0x40u),
              "hiding releases touch A while preserving controller Up");
        held = finger(SDL_FINGERMOTION, 1, 0.945f, 0.785f);
        check((held.keyinput & 1u) && !(held.keyinput & 0x40u),
              "a held finger cannot reactivate a hidden button after moving");
        finger(SDL_FINGERUP, 1, 0.945f, 0.785f);
#if defined(GBARECOMP_TOUCH_CAPTURE)
        capture_next_present = true;
#endif
        touch.present(rgb.data());
#if defined(GBARECOMP_TOUCH_CAPTURE)
        check(!capture_rgba.empty(), "capture actual hidden-pad host chrome before presentation");
        if (!capture_rgba.empty()) {
            const auto pixel = (std::size_t(capture_height * 0.061f) * capture_width + capture_width / 2) * 4;
            check(capture_rgba[pixel] || capture_rgba[pixel + 1] || capture_rgba[pixel + 2],
                  "Menu remains visibly drawn while every game button is hidden");
            std::ofstream image(dir / "menu-hidden.rgba", std::ios::binary);
            image.write(reinterpret_cast<const char*>(capture_rgba.data()), capture_rgba.size());
            std::printf("Hidden-pad Menu capture: %dx%d RGBA\n", capture_width, capture_height);
        }
#endif
        finger(SDL_FINGERDOWN, 3, 0.5f, 0.061f);
        check(recomp_runtime_ui_is_open(ui), "tapping visible Menu opens Settings with hidden buttons");
        finger(SDL_FINGERUP, 3, 0.5f, 0.061f);
        check(!touch.touch_pad_visible(), "opening Menu cannot click the new Settings row accidentally");
        recomp_runtime_ui_close(ui);

        SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP, 0);
        SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK, 1);
        SDL_JoystickUpdate(); touch.pump();
        check(recomp_runtime_ui_is_open(ui), "physical controller menu works with hidden buttons");
        SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK, 0);
        SDL_JoystickUpdate(); touch.pump(); recomp_runtime_ui_close(ui);
        SDL_Delay(300); // Let the existing 250ms synthetic-mouse handoff expire.
        finger(SDL_FINGERDOWN, 2, 0.5f, 0.5f);
        auto& hub = gbarecomp::TouchHub::instance();
        const auto points = hub.points();
        check(points.size() == 1 && points.front().pointer == 2,
              "hidden-pad finger is routed to the existing gesture recognizer");
        if (points.size() == 1) {
            // Exercise the real recognizer's deadline without depending on a
            // wall-clock sleep landing on the same scheduler tick.
            hub.advance(points.front().down_ms + hub.gesture_config().long_press_ms);
            touch.pump();
        }
        if (!recomp_runtime_ui_is_open(ui))
            std::fprintf(stderr, "long press: %s\n", hub.diagnostics_json().c_str());
        check(recomp_runtime_ui_is_open(ui), "native long-press deadline opens Settings with hidden buttons");
        finger(SDL_FINGERUP, 2, 0.5f, 0.5f);
        recomp_runtime_ui_close(ui); touch.set_runtime_ui(nullptr); recomp_runtime_ui_destroy(ui);
    }
    {
        gbarecomp::HostWindow touch; check(touch.open(3), "reopen hidden session");
        touch.set_touch_config_dir(dir.string().c_str()); touch.configure_touch(false, 0, 1, true);
        check(!touch.touch_pad_visible(), "saved Off survives restart and a visible title default");
        std::ifstream file(dir / "config.ini"); const std::string saved((std::istreambuf_iterator<char>(file)), {});
        check(saved.find("Keep=73") != std::string::npos, "unrelated settings preserved");
        check(gbarecomp::touch_controls_ui_set(&touch, 1), "save explicit On");
    }
    {
        gbarecomp::HostWindow touch; check(touch.open(3), "reopen visible session");
        touch.set_touch_config_dir(dir.string().c_str()); touch.configure_touch(false, 0, 0, true);
        check(touch.touch_pad_visible(), "saved On survives restart and a hidden title default");
    }
    SDL_JoystickClose(pad); SDL_JoystickDetachVirtual(device); SDL_Quit();
    std::printf("touch controls contract: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
#endif
