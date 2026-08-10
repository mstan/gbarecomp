// host_window.cpp — SDL2-backed window. Stubs out when SDL2 isn't found.

#include "host_window.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "color_lut.h"
#include "presentation_layout.h"
#if defined(GBARECOMP_RUNTIME_UI)
#include "recomp_runtime_ui.h"
#endif

#if defined(GBARECOMP_HAVE_SDL2)

#include <SDL.h>

#if defined(GBARECOMP_RUNTIME_UI)
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#if !SDL_VERSION_ATLEAST(2, 0, 17)
#error "The recomp-ui SDL_Renderer2 runtime adapter requires SDL 2.0.17 or newer"
#endif
#endif

// Shared ecosystem clock-domain bridge (callback-driven DRC). Replaces the
// SDL_QueueAudio push path, which silence-filled on queue underrun (~3.4/s
// measured on Minish Cap) and hard-flushed on overflow — the same output-side
// crackle fixed on NES. IMPL is defined in exactly this one translation unit.
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace gbarecomp {

namespace {

// System hotkey ids (config.ini [KeyMap] rows this host implements). Reset,
// PauseDimmed and ToggleRenderer are intentionally absent — gbarecomp has no
// in-process reset, no attract-dim, and one renderer; the launcher's GBA
// hotkey catalog omits them to match.
enum HostHotkey {
    HK_FULLSCREEN = 0, HK_PAUSE, HK_TURBO,
    HK_WINDOW_BIGGER, HK_WINDOW_SMALLER,
    HK_VOLUME_UP, HK_VOLUME_DOWN, HK_DISPLAY_PERF,
    HK_SOLAR_BRIGHTER, HK_SOLAR_DIMMER, HK_SOLAR_LIVE,
    HK_COUNT
};

struct HotkeyBind {
    SDL_Keycode key = SDLK_UNKNOWN;   // SDLK_UNKNOWN = unbound
    Uint16      mods = 0;             // required KMOD_CTRL/ALT/SHIFT bits
};

constexpr int assist_pad_axis(int code, bool positive) {
    return 100 + code * 2 + (positive ? 1 : 0);
}

bool assist_pad_down(SDL_GameController* controller, int binding) {
    if (!controller || !SDL_GameControllerGetAttached(controller)) return false;
    if (binding > 0 && binding < 100) {
        const int code = binding - 1;
        return code >= 0 && code < SDL_CONTROLLER_BUTTON_MAX &&
               SDL_GameControllerGetButton(
                   controller, static_cast<SDL_GameControllerButton>(code));
    }
    if (binding >= 100) {
        const int encoded = binding - 100;
        const int code = encoded / 2;
        const bool positive = (encoded & 1) != 0;
        if (code < 0 || code >= SDL_CONTROLLER_AXIS_MAX) return false;
        const Sint16 value = SDL_GameControllerGetAxis(
            controller, static_cast<SDL_GameControllerAxis>(code));
        return positive ? value >= 16000 : value <= -16000;
    }
    return false;
}

// ── MC-WS-002 present-cadence ring ───────────────────────────────────────
// Always-on (Release too) ring recording EVERY SDL_RenderPresent from window
// open: wall time blocked inside the call, entry-to-entry gap, and the DWM
// composition refresh counter (cRefresh) at exit — the scanout-side ruler
// that delivered-content framedumps cannot see. The ring records
// unconditionally (~24 B/present); GBARECOMP_PRESENT_CADENCE=1 adds a
// periodic stderr summary and a full CSV dump at close
// (GBARECOMP_PRESENT_CADENCE_DUMP=path overrides ./_present_cadence.csv).
// Interpretation (tools/analyze_present_cadence.py automates this):
//   block_us ≈ 0 on every present → vsync is NOT blocking (tear-prone);
//   rdelta 1,1,1… at ~16.74 ms gaps on a high-Hz VRR panel → VRR engaged;
//   rdelta 2/3 alternation on 164 Hz → 59.73→164 pulldown (cadence judder);
//   rdelta 0 rows → two presents inside one refresh (frame replaced).
struct PresentSample {
    uint64_t qpc = 0;          // SDL performance counter at present entry
    uint64_t dwm_refresh = 0;  // DWM cRefresh after present (0 = unavailable)
    uint32_t block_us = 0;     // wall time inside SDL_RenderPresent
    uint32_t gap_us = 0;       // entry-to-entry gap from the previous present
    uint8_t  fullscreen = 0;   // window was fullscreen for this present
};

constexpr int kCadenceRingSize     = 16384;  // ~4.5 min at 60 presents/s
constexpr int kCadenceSummaryEvery = 360;    // ~6 s between stderr summaries

struct PresentCadence {
    std::vector<PresentSample> ring;
    uint64_t total = 0;        // lifetime presents (ring keeps the last N)
    uint64_t qpc_freq = 0;
    uint64_t last_qpc = 0;     // previous present entry (0 = none yet)
    bool verbose = false;
    std::string dump_path;
#if defined(_WIN32)
    HRESULT (WINAPI* dwm_gcti)(HWND, DWM_TIMING_INFO*) = nullptr;
#endif

    void init() {
        ring.resize(kCadenceRingSize);
        qpc_freq = SDL_GetPerformanceFrequency();
        const char* e = std::getenv("GBARECOMP_PRESENT_CADENCE");
        verbose = e && *e && *e != '0';
        const char* d = std::getenv("GBARECOMP_PRESENT_CADENCE_DUMP");
        dump_path = (d && *d) ? d : "_present_cadence.csv";
#if defined(_WIN32)
        if (HMODULE m = LoadLibraryA("dwmapi.dll")) {
            dwm_gcti = reinterpret_cast<HRESULT (WINAPI*)(HWND, DWM_TIMING_INFO*)>(
                reinterpret_cast<void*>(
                    GetProcAddress(m, "DwmGetCompositionTimingInfo")));
        }
#endif
    }

    // Query the always-on DWM composition clock: refresh counter now, and
    // (optionally) the compositor's nominal refresh rate in Hz.
    uint64_t dwm_refresh_now(double* rate_hz) {
#if defined(_WIN32)
        if (dwm_gcti) {
            DWM_TIMING_INFO ti{};
            ti.cbSize = sizeof(ti);
            if (SUCCEEDED(dwm_gcti(nullptr, &ti))) {
                if (rate_hz)
                    *rate_hz = ti.rateRefresh.uiDenominator
                        ? static_cast<double>(ti.rateRefresh.uiNumerator) /
                              static_cast<double>(ti.rateRefresh.uiDenominator)
                        : 0.0;
                return ti.cRefresh;
            }
        }
#endif
        if (rate_hz) *rate_hz = 0.0;
        return 0;
    }

    uint32_t to_us(uint64_t qpc_delta) const {
        if (!qpc_freq) return 0;
        const uint64_t us = qpc_delta * 1000000ull / qpc_freq;
        return us > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(us);
    }

    void record(uint64_t qpc0, uint64_t qpc1, bool fullscreen) {
        PresentSample s;
        s.qpc = qpc0;
        s.block_us = to_us(qpc1 - qpc0);
        s.gap_us = last_qpc ? to_us(qpc0 - last_qpc) : 0;
        last_qpc = qpc0;
        s.dwm_refresh = dwm_refresh_now(nullptr);
        s.fullscreen = fullscreen ? 1 : 0;
        ring[total % kCadenceRingSize] = s;
        ++total;
        if (verbose && (total % kCadenceSummaryEvery) == 0) summarize();
    }

    void summarize() {
        const int n = static_cast<int>(
            std::min<uint64_t>(total, kCadenceSummaryEvery));
        if (n < 2) return;
        std::vector<uint32_t> gaps, blocks;
        gaps.reserve(n);
        blocks.reserve(n);
        int hist[7] = {};  // rdelta 0..4, [5]=5+, [6]=n/a
        uint64_t prev_refresh = 0;
        bool have_prev = false;
        uint64_t gap_sum = 0;
        int fs = 0;
        for (int i = n; i >= 1; --i) {
            const PresentSample& s = ring[(total - i) % kCadenceRingSize];
            blocks.push_back(s.block_us);
            if (s.gap_us) { gaps.push_back(s.gap_us); gap_sum += s.gap_us; }
            fs += s.fullscreen;
            if (s.dwm_refresh == 0) {
                ++hist[6];
                have_prev = false;
            } else {
                if (have_prev) {
                    const uint64_t d = s.dwm_refresh - prev_refresh;
                    ++hist[d >= 5 ? 5 : static_cast<int>(d)];
                }
                prev_refresh = s.dwm_refresh;
                have_prev = true;
            }
        }
        auto pct = [](std::vector<uint32_t>& v, double p) -> uint32_t {
            if (v.empty()) return 0;
            const size_t k = static_cast<size_t>(p * (v.size() - 1));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return v[k];
        };
        const uint32_t bmax = blocks.empty()
            ? 0 : *std::max_element(blocks.begin(), blocks.end());
        const uint32_t b95 = pct(blocks, 0.95);
        const uint32_t b50 = pct(blocks, 0.50);
        const uint32_t g50 = pct(gaps, 0.50);
        double rate_hz = 0.0;
        dwm_refresh_now(&rate_hz);
        const double fps = gap_sum
            ? static_cast<double>(gaps.size()) * 1e6 / static_cast<double>(gap_sum)
            : 0.0;
        std::fprintf(stderr,
            "[present-cadence] n=%llu fps=%.2f block_us p50=%u p95=%u max=%u "
            "gap_us p50=%u rdel{0:%d 1:%d 2:%d 3:%d 4:%d 5+:%d na:%d} "
            "dwm_hz=%.2f fs=%d/%d\n",
            static_cast<unsigned long long>(total), fps, b50, b95, bmax, g50,
            hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6],
            rate_hz, fs, n);
        std::fflush(stderr);
    }

    // Full-ring CSV dump (verbose only) — the queryable record of the run.
    void dump() {
        if (!verbose || total == 0) return;
        std::FILE* f = std::fopen(dump_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "[present-cadence] cannot write %s\n",
                         dump_path.c_str());
            return;
        }
        std::fprintf(f, "idx,t_ms,gap_us,block_us,dwm_refresh,rdelta,fullscreen\n");
        const uint64_t n = std::min<uint64_t>(total, kCadenceRingSize);
        const uint64_t first_qpc = ring[(total - n) % kCadenceRingSize].qpc;
        uint64_t prev_refresh = 0;
        bool have_prev = false;
        for (uint64_t i = 0; i < n; ++i) {
            const PresentSample& s = ring[(total - n + i) % kCadenceRingSize];
            long long rdelta = -1;
            if (s.dwm_refresh) {
                if (have_prev)
                    rdelta = static_cast<long long>(s.dwm_refresh - prev_refresh);
                prev_refresh = s.dwm_refresh;
                have_prev = true;
            }
            std::fprintf(f, "%llu,%.3f,%u,%u,%llu,%lld,%u\n",
                static_cast<unsigned long long>(total - n + i),
                qpc_freq ? static_cast<double>(s.qpc - first_qpc) * 1000.0 /
                               static_cast<double>(qpc_freq)
                         : 0.0,
                s.gap_us, s.block_us,
                static_cast<unsigned long long>(s.dwm_refresh), rdelta,
                static_cast<unsigned>(s.fullscreen));
        }
        std::fclose(f);
        std::fprintf(stderr, "[present-cadence] dumped %llu presents -> %s\n",
                     static_cast<unsigned long long>(n), dump_path.c_str());
        std::fflush(stderr);
    }
};

// One-line display-mode report (index, geometry, nominal Hz) so cadence data
// can be interpreted against the panel the window actually sits on.
void log_display_mode(SDL_Window* win, const char* tag) {
    if (!win) return;
    const int di = SDL_GetWindowDisplayIndex(win);
    SDL_DisplayMode dm{};
    if (di >= 0 && SDL_GetCurrentDisplayMode(di, &dm) == 0) {
        std::fprintf(stderr, "host_window: %s display=%d mode=%dx%d@%dHz\n",
                     tag, di, dm.w, dm.h, dm.refresh_rate);
        std::fflush(stderr);
    }
}

struct Backend {
    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture*  texture  = nullptr;
    SDL_Texture*  sharp_texture = nullptr;
    int           sharp_texture_w = 0;
    int           sharp_texture_h = 0;
    bool          sharp_target_failed = false;
    SDL_AudioDeviceID audio_dev = 0;
    SDL_GameController* controller = nullptr;
    SDL_JoystickID      controller_id = -1;
    bool                controller_gyro = false;
    SDL_Sensor*         device_gyro = nullptr;
    bool                device_gyro_motion_logged = false;
    struct TouchPoint {
        SDL_FingerID id = 0;
        float x = 0.0f;
        float y = 0.0f;
        float start_x = 0.0f;
        float start_y = 0.0f;
        Uint32 started_at = 0;
        uint16_t buttons = 0;
        bool active = false;
        bool long_press_candidate = false;
    };
    std::array<TouchPoint, 10> touches{};
    bool touch_controls = false;
    // Callback-driven clock-domain bridge (replaces SDL queue push).
    rab_bridge    bridge{};
    bool          bridge_ready = false;
    SDL_mutex*    audio_mtx = nullptr;
    // Diagnostic/direct path: let SDL own format conversion and queue the
    // engine's verified 65536 Hz mono stream without passing it through RAB.
    bool          audio_direct = false;
    bool          audio_direct_started = false;
    int           audio_direct_rate = 65536;
    uint64_t      audio_direct_pushes = 0;
    uint64_t      audio_direct_samples = 0;
    uint64_t      audio_direct_underruns = 0;
    uint64_t      audio_direct_probe_samples = 0;
    uint64_t      audio_direct_probe_nonzero = 0;
    long double   audio_direct_probe_square_sum = 0.0;
    int           audio_direct_probe_peak = 0;
    Uint32        audio_direct_min_bytes = UINT32_MAX;
    Uint32        audio_direct_max_bytes = 0;
    // Present-time screen-color simulation. Built once from
    // GBARECOMP_SCREEN; default Raw = exact passthrough (no copy, no
    // grading), so default behavior is byte-identical to upstream.
    std::unique_ptr<runtime::ColorLut> color_lut;
    std::vector<uint8_t> graded_fb;  // scratch RGB888 (base_w*base_h*3)
#if defined(GBARECOMP_RUNTIME_UI)
    RecompRuntimeUi* runtime_ui = nullptr;
    ImGuiContext* runtime_imgui_context = nullptr;
    bool runtime_imgui_ready = false;
    // A stationary long press opens the runtime menu. Its eventual FINGERUP
    // must not become a click on whichever menu row appeared under the finger.
    int runtime_ui_suppressed_touch_releases = 0;
    Uint32 runtime_ui_suppress_touch_until = 0;
#endif
    int base_w = 240;   // logical surface width  (240 faithful, wider if expanded)
    int base_h = 160;   // logical surface height (160; vertical expansion deferred)
    bool expanded_view = false;  // native games retain the historical SDL path
    bool resize_driven_view = false;
    bool freely_resizable_window = false;
    bool linear_filter = false;
    bool sharp_filter = false;

    // ---- rebindable input (see HostWindow::load_input_config) --------------
    // GBA KEYINPUT bit (0..9) per bound SDL scancode; seeded from the
    // built-in defaults, overridden by keybinds.ini [player1].
    SDL_Scancode bind_sc[10] = {};      // indexed by GBA KEYINPUT bit
    HotkeyBind   hotkeys[HK_COUNT];     // [KeyMap] bindings
    SDL_Scancode assist_key[2] = {
        SDL_SCANCODE_1, SDL_SCANCODE_2
    };                                  // Rewind, Fast-forward
    int          assist_pad[2] = {
        assist_pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true),
        assist_pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true),
    };
    bool         assist_tools = true;
    int          assist_fast_forward_multiplier = 4;
    bool         assist_rewind_was_down = false;
    Uint32       assist_rewind_repeat_at = 0;
    int          scale = 3;             // current integer window scale
    int          fullscreen = 0;        // 0 windowed, 1 borderless, 2 exclusive
    int          volume = 100;          // 0..100 gain on pushed samples
    std::vector<int16_t> volume_buf;    // scratch for gain != 100
    // FPS readout (DisplayPerf hotkey): presents/sec in the title bar.
    bool         fps_readout = false;
    std::string  title;                 // base window title (readout restores it)
    Uint32       fps_window_start = 0;
    int          fps_presents = 0;
    // MC-WS-002: always-on per-present timing/scanout ring (see above).
    PresentCadence cadence;
};

void destroy_sharp_texture(Backend* b) {
    if (!b) return;
    if (b->sharp_texture) SDL_DestroyTexture(b->sharp_texture);
    b->sharp_texture = nullptr;
    b->sharp_texture_w = 0;
    b->sharp_texture_h = 0;
}

bool ensure_sharp_texture(Backend* b, int factor) {
    if (!b || !b->renderer || factor < 2) return false;
    const int width = b->base_w * factor;
    const int height = b->base_h * factor;
    if (b->sharp_texture &&
        b->sharp_texture_w == width &&
        b->sharp_texture_h == height) {
        return true;
    }

    SDL_RendererInfo info{};
    if (SDL_GetRendererInfo(b->renderer, &info) == 0 &&
        ((info.max_texture_width > 0 && width > info.max_texture_width) ||
         (info.max_texture_height > 0 && height > info.max_texture_height))) {
        if (!b->sharp_target_failed) {
            std::fprintf(stderr,
                         "host_window: sharp scaler target %dx%d exceeds "
                         "renderer limit %dx%d; using nearest fallback\n",
                         width, height, info.max_texture_width,
                         info.max_texture_height);
            b->sharp_target_failed = true;
        }
        destroy_sharp_texture(b);
        return false;
    }

    destroy_sharp_texture(b);
    b->sharp_texture = SDL_CreateTexture(
        b->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
        width, height);
    if (!b->sharp_texture) {
        if (!b->sharp_target_failed) {
            std::fprintf(stderr,
                         "host_window: sharp scaler target creation failed: "
                         "%s; using nearest fallback\n",
                         SDL_GetError());
            b->sharp_target_failed = true;
        }
        return false;
    }
    b->sharp_texture_w = width;
    b->sharp_texture_h = height;
    b->sharp_target_failed = false;
    SDL_SetTextureBlendMode(b->sharp_texture, SDL_BLENDMODE_NONE);
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(b->sharp_texture, SDL_ScaleModeLinear);
#endif
    return true;
}

void close_game_controller(Backend* b) {
    if (!b || !b->controller) return;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (b->controller_gyro)
        SDL_GameControllerSetSensorEnabled(
            b->controller, SDL_SENSOR_GYRO, SDL_FALSE);
#endif
    SDL_GameControllerClose(b->controller);
    b->controller = nullptr;
    b->controller_id = -1;
    b->controller_gyro = false;
}

bool open_game_controller(Backend* b, int device_index) {
    if (!b || b->controller || device_index < 0 ||
        !SDL_IsGameController(device_index))
        return false;

    SDL_GameController* controller = SDL_GameControllerOpen(device_index);
    if (!controller) return false;
    b->controller = controller;
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
    b->controller_id = joystick ? SDL_JoystickInstanceID(joystick) : -1;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) &&
        SDL_GameControllerSetSensorEnabled(
            controller, SDL_SENSOR_GYRO, SDL_TRUE) == 0) {
        b->controller_gyro = true;
    }
#endif
    std::fprintf(stderr,
                 "host_window: controller=%s id=%d gyro=%s\n",
                 SDL_GameControllerName(controller)
                     ? SDL_GameControllerName(controller) : "unknown",
                 static_cast<int>(b->controller_id),
                 b->controller_gyro ? "enabled" : "unavailable");
    std::fflush(stderr);
    return true;
}

void open_first_game_controller(Backend* b) {
    if (!b || b->controller) return;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (open_game_controller(b, i)) return;
    }
}

void open_device_gyro(Backend* b) {
    if (!b || b->device_gyro) return;
#if SDL_VERSION_ATLEAST(2, 0, 9)
    for (int i = 0; i < SDL_NumSensors(); ++i) {
        if (SDL_SensorGetDeviceType(i) != SDL_SENSOR_GYRO) continue;
        b->device_gyro = SDL_SensorOpen(i);
        if (b->device_gyro) {
            std::fprintf(stderr, "host_window: device gyro=%s\n",
                         SDL_SensorGetName(b->device_gyro)
                             ? SDL_SensorGetName(b->device_gyro) : "available");
            std::fflush(stderr);
        }
        break;
    }
#endif
}

// Normalized landscape touch layout. It deliberately leaves the center of the
// screen empty so a stationary long press there can open runtime settings.
uint16_t touch_buttons_at(float x, float y) {
    auto in_circle = [x, y](float cx, float cy, float r) {
        const float dx = x - cx;
        const float dy = y - cy;
        return dx * dx + dy * dy <= r * r;
    };

    uint16_t buttons = 0;
#if defined(__ANDROID__)
    if (x < 0.27f && y > 0.39f) {
        const float dx = x - 0.12f;
        const float dy = y - 0.69f;
        if (std::abs(dx) > std::abs(dy)) {
            if (dx > 0.028f) buttons |= 1u << 4;  // Right
            if (dx < -0.028f) buttons |= 1u << 5; // Left
        } else {
            if (dy < -0.028f) buttons |= 1u << 6; // Up
            if (dy > 0.028f) buttons |= 1u << 7;  // Down
        }
    }
    if (in_circle(0.90f, 0.62f, 0.085f)) buttons |= 1u << 0; // A
    if (in_circle(0.83f, 0.78f, 0.085f)) buttons |= 1u << 1; // B
    if (x >= 0.39f && x <= 0.48f && y >= 0.88f) buttons |= 1u << 2; // Select
    if (x >= 0.52f && x <= 0.61f && y >= 0.88f) buttons |= 1u << 3; // Start
    if (x <= 0.22f && y <= 0.17f) buttons |= 1u << 9; // L
    if (x >= 0.78f && y <= 0.17f) buttons |= 1u << 8; // R
#else
    if (x < 0.31f && y > 0.37f) {
        const float dx = x - 0.17f;
        const float dy = y - 0.70f;
        if (std::abs(dx) > std::abs(dy)) {
            if (dx > 0.035f) buttons |= 1u << 4;  // Right
            if (dx < -0.035f) buttons |= 1u << 5; // Left
        } else {
            if (dy < -0.035f) buttons |= 1u << 6; // Up
            if (dy > 0.035f) buttons |= 1u << 7;  // Down
        }
    }
    if (in_circle(0.87f, 0.62f, 0.095f)) buttons |= 1u << 0; // A
    if (in_circle(0.74f, 0.76f, 0.095f)) buttons |= 1u << 1; // B
    if (x >= 0.39f && x <= 0.48f && y >= 0.84f) buttons |= 1u << 2; // Select
    if (x >= 0.52f && x <= 0.61f && y >= 0.84f) buttons |= 1u << 3; // Start
    if (x <= 0.24f && y <= 0.16f) buttons |= 1u << 9; // L
    if (x >= 0.76f && y <= 0.16f) buttons |= 1u << 8; // R
#endif
    return buttons;
}

Backend::TouchPoint* find_touch(Backend* b, SDL_FingerID id) {
    if (!b) return nullptr;
    for (auto& touch : b->touches)
        if (touch.active && touch.id == id) return &touch;
    return nullptr;
}

Backend::TouchPoint* allocate_touch(Backend* b, SDL_FingerID id) {
    if (!b) return nullptr;
    if (auto* existing = find_touch(b, id)) return existing;
    for (auto& touch : b->touches) {
        if (!touch.active) {
            touch = {};
            touch.active = true;
            touch.id = id;
            return &touch;
        }
    }
    return nullptr;
}

uint16_t active_touch_buttons(const Backend* b) {
    uint16_t buttons = 0;
    if (!b) return buttons;
    for (const auto& touch : b->touches)
        if (touch.active) buttons |= touch.buttons;
    return buttons;
}

void clear_touches(Backend* b) {
    if (!b) return;
    for (auto& touch : b->touches) touch = {};
}

void fill_circle(SDL_Renderer* renderer, int cx, int cy, int radius) {
    for (int y = -radius; y <= radius; ++y) {
        const int half = static_cast<int>(
            std::sqrt(static_cast<float>(radius * radius - y * y)));
        SDL_RenderDrawLine(renderer, cx - half, cy + y, cx + half, cy + y);
    }
}

void render_touch_controls(Backend* b) {
    if (!b || !b->touch_controls || !b->renderer) return;
#if defined(GBARECOMP_RUNTIME_UI)
    if (b->runtime_ui && recomp_runtime_ui_is_open(b->runtime_ui)) return;
#endif
    int w = b->base_w;
    int h = b->base_h;
#if defined(__ANDROID__)
    if (SDL_GetRendererOutputSize(b->renderer, &w, &h) != 0 ||
        w <= 0 || h <= 0) {
        SDL_GetWindowSize(b->window, &w, &h);
    }
#endif
    const uint16_t held = active_touch_buttons(b);
    SDL_SetRenderDrawBlendMode(b->renderer, SDL_BLENDMODE_BLEND);

    auto color_for = [b, held](int bit) {
        if (held & (1u << bit))
            SDL_SetRenderDrawColor(b->renderer, 255, 214, 74, 220);
        else if (bit == 0)
            SDL_SetRenderDrawColor(b->renderer, 255, 92, 98, 148);
        else if (bit == 1)
            SDL_SetRenderDrawColor(b->renderer, 83, 196, 255, 148);
        else
            SDL_SetRenderDrawColor(b->renderer, 224, 234, 246, 92);
    };
#if defined(__ANDROID__)
    const int dpad_x = static_cast<int>(w * 0.12f);
    const int dpad_y = static_cast<int>(h * 0.69f);
    const int arm = std::max(42, static_cast<int>(h * 0.105f));
    const int thick = std::max(30, static_cast<int>(h * 0.050f));
    SDL_SetRenderDrawColor(b->renderer, 224, 234, 246, 82);
    SDL_Rect vertical{dpad_x - thick / 2, dpad_y - arm,
                      thick, arm * 2};
    SDL_Rect horizontal{dpad_x - arm, dpad_y - thick / 2,
                        arm * 2, thick};
    SDL_RenderFillRect(b->renderer, &vertical);
    SDL_RenderFillRect(b->renderer, &horizontal);

    const int button_r = std::max(34, static_cast<int>(h * 0.060f));
    color_for(0);
    fill_circle(b->renderer, static_cast<int>(w * 0.90f),
                static_cast<int>(h * 0.62f), button_r);
    color_for(1);
    fill_circle(b->renderer, static_cast<int>(w * 0.83f),
                static_cast<int>(h * 0.78f), button_r);

    SDL_SetRenderDrawColor(b->renderer, 224, 234, 246, 70);
    const int shoulder_w = static_cast<int>(w * 0.17f);
    const int shoulder_h = std::max(28, static_cast<int>(h * 0.052f));
    const int shoulder_pad = std::max(16, static_cast<int>(w * 0.015f));
    SDL_Rect left_shoulder{shoulder_pad, static_cast<int>(h * 0.055f),
                           shoulder_w, shoulder_h};
    SDL_Rect right_shoulder{w - shoulder_pad - shoulder_w,
                            static_cast<int>(h * 0.055f),
                            shoulder_w, shoulder_h};
    SDL_RenderFillRect(b->renderer, &left_shoulder);
    SDL_RenderFillRect(b->renderer, &right_shoulder);

    const int pill_w = std::max(58, static_cast<int>(w * 0.055f));
    const int pill_h = std::max(12, static_cast<int>(h * 0.018f));
    SDL_Rect select{static_cast<int>(w * 0.435f) - pill_w / 2,
                    static_cast<int>(h * 0.925f), pill_w, pill_h};
    SDL_Rect start{static_cast<int>(w * 0.565f) - pill_w / 2,
                   static_cast<int>(h * 0.925f), pill_w, pill_h};
    SDL_RenderFillRect(b->renderer, &select);
    SDL_RenderFillRect(b->renderer, &start);
#else
    const int dpad_x = static_cast<int>(w * 0.17f);
    const int dpad_y = static_cast<int>(h * 0.70f);
    const int arm = std::max(8, h / 11);
    const int thick = std::max(6, h / 17);
    SDL_SetRenderDrawColor(b->renderer, 235, 245, 255, 70);
    SDL_Rect vertical{dpad_x - thick / 2, dpad_y - arm,
                      thick, arm * 2};
    SDL_Rect horizontal{dpad_x - arm, dpad_y - thick / 2,
                        arm * 2, thick};
    SDL_RenderFillRect(b->renderer, &vertical);
    SDL_RenderFillRect(b->renderer, &horizontal);

    const int button_r = std::max(7, h / 18);
    color_for(0);
    fill_circle(b->renderer, static_cast<int>(w * 0.87f),
                static_cast<int>(h * 0.62f), button_r);
    color_for(1);
    fill_circle(b->renderer, static_cast<int>(w * 0.74f),
                static_cast<int>(h * 0.76f), button_r);

    SDL_SetRenderDrawColor(b->renderer, 235, 245, 255, 70);
    SDL_Rect left_shoulder{0, 0, static_cast<int>(w * 0.24f),
                           std::max(5, h / 14)};
    SDL_Rect right_shoulder{static_cast<int>(w * 0.76f), 0,
                            static_cast<int>(w * 0.24f),
                            std::max(5, h / 14)};
    SDL_RenderFillRect(b->renderer, &left_shoulder);
    SDL_RenderFillRect(b->renderer, &right_shoulder);
    SDL_Rect select{static_cast<int>(w * 0.40f), static_cast<int>(h * 0.87f),
                    static_cast<int>(w * 0.07f), std::max(3, h / 32)};
    SDL_Rect start{static_cast<int>(w * 0.53f), static_cast<int>(h * 0.87f),
                   static_cast<int>(w * 0.07f), std::max(3, h / 32)};
    SDL_RenderFillRect(b->renderer, &select);
    SDL_RenderFillRect(b->renderer, &start);
#endif

    for (const auto& touch : b->touches) {
        if (!touch.active || !touch.long_press_candidate) continue;
        const Uint32 elapsed = SDL_GetTicks() - touch.started_at;
        const float progress =
            std::min(1.0f, static_cast<float>(elapsed) / 650.0f);
        SDL_SetRenderDrawColor(b->renderer, 80, 210, 255, 190);
        SDL_Rect bar{static_cast<int>(w * 0.30f), std::max(2, h / 30),
                     static_cast<int>(w * 0.40f * progress),
                     std::max(2, h / 45)};
        SDL_RenderFillRect(b->renderer, &bar);
    }
    SDL_SetRenderDrawBlendMode(b->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(b->renderer, 7, 11, 20, 255);
}

// GBA KEYINPUT bit order: 0=A 1=B 2=Sel 3=Sta 4=Right 5=Left 6=Up 7=Down 8=R 9=L.
// Defaults MATCH recomp-ui's generic keybinds defaults (keybinds.c) so the
// launcher's rebind page and the game agree even before keybinds.ini exists:
// A=X, B=Z, L=C, R=V, Start=Return, Select=RShift, D-pad=arrows.
// (This supersedes the pre-launcher hardcoded Z/X/A/S layout — one defaults
// source across the recomp ecosystem; rebind in the launcher to taste.)
const SDL_Scancode kDefaultBinds[10] = {
    SDL_SCANCODE_X,       // A
    SDL_SCANCODE_Z,       // B
    SDL_SCANCODE_RSHIFT,  // Select
    SDL_SCANCODE_RETURN,  // Start
    SDL_SCANCODE_RIGHT,   // Right
    SDL_SCANCODE_LEFT,    // Left
    SDL_SCANCODE_UP,      // Up
    SDL_SCANCODE_DOWN,    // Down
    SDL_SCANCODE_V,       // R
    SDL_SCANCODE_C,       // L
};

// keybinds.ini [player1] key name -> GBA KEYINPUT bit. x/y/l2/r2/l3/r3 from
// the generic 16-slot format are ignored (no GBA equivalent).
const struct { const char* name; int bit; } kBindKeys[] = {
    { "a", 0 }, { "b", 1 }, { "select", 2 }, { "start", 3 },
    { "right", 4 }, { "left", 5 }, { "up", 6 }, { "down", 7 },
    { "r", 8 }, { "l", 9 },
};

// config.ini [KeyMap] key names, in HostHotkey order, with the same defaults
// recomp-ui's hotkey panel displays for the GBA catalog.
const char* const kHotkeyNames[HK_COUNT] = {
    "Fullscreen", "Pause", "Turbo",
    "WindowBigger", "WindowSmaller",
    "VolumeUp", "VolumeDown", "DisplayPerf",
    "SolarBrighter", "SolarDimmer", "SolarLive",
};
const char* const kHotkeyDefaults[HK_COUNT] = {
    "Alt+Return", "Shift+P", "Tab",
    "", "",
    "", "", "F",
    "", "", "",
};

// SDL_GetScancodeFromName plus the same lowercase aliases recomp-ui's
// keybinds.c accepts, so a file either side writes round-trips identically.
SDL_Scancode scancode_from_name(const char* name) {
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN) return sc;
    std::string b;
    for (const char* p = name; *p; ++p) b += (char)std::tolower((unsigned char)*p);
    if (b == "enter" || b == "return") return SDL_SCANCODE_RETURN;
    if (b == "tab")       return SDL_SCANCODE_TAB;
    if (b == "space")     return SDL_SCANCODE_SPACE;
    if (b == "lshift")    return SDL_SCANCODE_LSHIFT;
    if (b == "rshift")    return SDL_SCANCODE_RSHIFT;
    if (b == "lctrl")     return SDL_SCANCODE_LCTRL;
    if (b == "rctrl")     return SDL_SCANCODE_RCTRL;
    if (b == "lalt")      return SDL_SCANCODE_LALT;
    if (b == "ralt")      return SDL_SCANCODE_RALT;
    if (b == "backslash") return SDL_SCANCODE_BACKSLASH;
    if (b == "escape" || b == "esc") return SDL_SCANCODE_ESCAPE;
    if (b == "backspace") return SDL_SCANCODE_BACKSPACE;
    return SDL_SCANCODE_UNKNOWN;
}

// Parse a [KeyMap] value ("Ctrl+R", "Alt+Return", "F", "" = unbound) into a
// HotkeyBind. Mirrors the format recomp-ui's hotkey editor writes
// (SDL keycode name with Ctrl+/Alt+/Shift+ prefixes).
HotkeyBind parse_hotkey(const char* value) {
    HotkeyBind hb;
    if (!value || !*value) return hb;
    std::string v = value;
    Uint16 mods = 0;
    for (;;) {
        if (v.rfind("Ctrl+", 0) == 0)       { mods |= KMOD_CTRL;  v.erase(0, 5); }
        else if (v.rfind("Alt+", 0) == 0)   { mods |= KMOD_ALT;   v.erase(0, 4); }
        else if (v.rfind("Shift+", 0) == 0) { mods |= KMOD_SHIFT; v.erase(0, 6); }
        else break;
    }
    SDL_Keycode k = SDL_GetKeyFromName(v.c_str());
    if (k == SDLK_UNKNOWN) return hb;   // unparseable = unbound
    hb.key = k;
    hb.mods = mods;
    return hb;
}

// True when `hb` is bound and its required modifiers are (all) held. Ctrl/
// Alt/Shift not required by the binding must NOT be held — so "P" and
// "Shift+P" stay distinct bindings.
bool hotkey_mods_ok(const HotkeyBind& hb, Uint16 state_mods) {
    auto want = [&](Uint16 m) { return (hb.mods & m) != 0; };
    auto held = [&](Uint16 m) { return (state_mods & m) != 0; };
    return want(KMOD_CTRL) == held(KMOD_CTRL) &&
           want(KMOD_ALT) == held(KMOD_ALT) &&
           want(KMOD_SHIFT) == held(KMOD_SHIFT);
}

// Minimal INI section scan shared by keybinds.ini and config.ini [KeyMap]:
// calls fn(key, value) for each assignment inside `section`.
template <typename Fn>
void ini_scan_section(const char* path, const char* section, Fn fn) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return;
    char line[512];
    bool in_section = false;
    while (std::fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        size_t n = std::strlen(s);
        while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
            s[--n] = '\0';
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char* close = std::strchr(s, ']');
            if (close) *close = '\0';
            in_section = SDL_strcasecmp(s + 1, section) == 0;
            continue;
        }
        if (!in_section) continue;
        char* eq = std::strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = s;
        char* val = eq + 1;
        size_t kl = std::strlen(key);
        while (kl && (key[kl-1] == ' ' || key[kl-1] == '\t')) key[--kl] = '\0';
        while (*val == ' ' || *val == '\t') ++val;
        char* hash = std::strchr(val, '#');
        if (hash) *hash = '\0';
        size_t vl = std::strlen(val);
        while (vl && (val[vl-1] == ' ' || val[vl-1] == '\t')) val[--vl] = '\0';
        fn(key, val);
    }
    std::fclose(f);
}

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

#if defined(GBARECOMP_RUNTIME_UI)
bool runtime_imgui_init(Backend* b) {
    IMGUI_CHECKVERSION();
    b->runtime_imgui_context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
#if defined(__ANDROID__)
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    io.FontGlobalScale = 1.75f;
    ImGui::GetStyle().ScaleAllSizes(1.75f);
    ImGui::GetStyle().TouchExtraPadding = ImVec2(5.0f, 5.0f);
#endif

    if (!ImGui_ImplSDL2_InitForSDLRenderer(b->window, b->renderer)) {
        std::fprintf(stderr,
                     "host_window: ImGui SDL2 platform initialization failed\n");
        ImGui::DestroyContext();
        b->runtime_imgui_context = nullptr;
        return false;
    }
    if (!ImGui_ImplSDLRenderer2_Init(b->renderer)) {
        std::fprintf(stderr,
                     "host_window: ImGui SDL_Renderer2 initialization failed\n");
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        b->runtime_imgui_context = nullptr;
        return false;
    }
    b->runtime_imgui_ready = true;
    return true;
}

void runtime_imgui_shutdown(Backend* b) {
    if (!b->runtime_imgui_ready) return;
    ImGui::SetCurrentContext(b->runtime_imgui_context);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    b->runtime_imgui_context = nullptr;
    b->runtime_imgui_ready = false;
}

void runtime_imgui_render(Backend* b) {
    if (!b->runtime_imgui_ready || !b->runtime_ui ||
        !recomp_runtime_ui_is_open(b->runtime_ui)) {
        return;
    }
    ImGui::SetCurrentContext(b->runtime_imgui_context);

    // Native GBA presentation uses a 240x160 SDL logical size. ImGui must see
    // and render into the full drawable, then the exact game state must be
    // restored before the next frame.
    int logical_w = 0;
    int logical_h = 0;
    SDL_Rect viewport{};
    SDL_RenderGetLogicalSize(b->renderer, &logical_w, &logical_h);
    SDL_RenderGetViewport(b->renderer, &viewport);
    SDL_RenderSetLogicalSize(b->renderer, 0, 0);
    SDL_RenderSetViewport(b->renderer, nullptr);

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(b->runtime_ui);
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), b->renderer);

    SDL_RenderSetLogicalSize(b->renderer, logical_w, logical_h);
    SDL_RenderSetViewport(b->renderer, &viewport);
}

bool runtime_ui_event(RecompRuntimeUi* ui, const SDL_Event& e) {
    if (!ui) return false;
    if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE &&
        !recomp_runtime_ui_is_open(ui)) {
        recomp_runtime_ui_open(ui);
        return true;
    }
    // A focused text field owns the keyboard. ImGui already saw this event
    // (ImGui_ImplSDL2_ProcessEvent runs first in pump()), so the field is
    // receiving the keystrokes; mapping them to navigation as well would move
    // the selection out from under the caret and letters would double as menu
    // input. Still swallow them so the guest never sees the player typing.
    //
    // Guarded because TEXT items post-date some recomp-ui pins: calling this
    // unconditionally would fail to link against an older submodule, which is
    // the same trap the gyro fields already avoid in launcher_seam.h.
#if defined(RECOMP_RUNTIME_UI_HAS_TEXT)
    if (recomp_runtime_ui_wants_text_input(ui)) return true;
#endif
    if (e.type != SDL_KEYDOWN && e.type != SDL_KEYUP &&
        e.type != SDL_CONTROLLERBUTTONDOWN && e.type != SDL_CONTROLLERBUTTONUP)
        return false;
    RecompRuntimeUiInput input;
    bool mapped = true;
    const int pressed = e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN;
    const int repeat = e.type == SDL_KEYDOWN ? e.key.repeat : 0;
    if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_ESCAPE: input = RECOMP_RUNTIME_UI_INPUT_BACK; break;
            case SDL_SCANCODE_UP: input = RECOMP_RUNTIME_UI_INPUT_UP; break;
            case SDL_SCANCODE_DOWN: input = RECOMP_RUNTIME_UI_INPUT_DOWN; break;
            case SDL_SCANCODE_LEFT: input = RECOMP_RUNTIME_UI_INPUT_LEFT; break;
            case SDL_SCANCODE_RIGHT: input = RECOMP_RUNTIME_UI_INPUT_RIGHT; break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_SPACE: input = RECOMP_RUNTIME_UI_INPUT_ACCEPT; break;
            default: mapped = false; break;
        }
    } else {
        switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_GUIDE:
                if (pressed) {
                    if (recomp_runtime_ui_is_open(ui)) recomp_runtime_ui_close(ui);
                    else recomp_runtime_ui_open(ui);
                }
                return true;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: input = RECOMP_RUNTIME_UI_INPUT_UP; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: input = RECOMP_RUNTIME_UI_INPUT_DOWN; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: input = RECOMP_RUNTIME_UI_INPUT_LEFT; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: input = RECOMP_RUNTIME_UI_INPUT_RIGHT; break;
            case SDL_CONTROLLER_BUTTON_A: input = RECOMP_RUNTIME_UI_INPUT_ACCEPT; break;
            case SDL_CONTROLLER_BUTTON_B: input = RECOMP_RUNTIME_UI_INPUT_BACK; break;
            default: mapped = false; break;
        }
    }
    if (mapped && recomp_runtime_ui_is_open(ui))
        recomp_runtime_ui_handle_input(ui, input, pressed, repeat);
    return recomp_runtime_ui_is_open(ui) || mapped;
}
#endif

}  // namespace

HostWindow::HostWindow() = default;

HostWindow::~HostWindow() {
    close();
}

bool HostWindow::is_available() { return true; }

bool HostWindow::open(int scale, int base_w, int base_h, const char* title,
                      const char* screen, bool linear_filter, bool sharp_filter,
                      bool resize_driven_view,
                      bool freely_resizable_window) {
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
    const Uint32 controller_flags =
        SDL_INIT_GAMECONTROLLER | SDL_INIT_SENSOR;
    if ((SDL_WasInit(controller_flags) & controller_flags) !=
        controller_flags) {
        // HIDAPI is required for DualSense motion data on Windows. This hint
        // is the SDL default, but setting it before subsystem init makes the
        // intended driver explicit when a global SDL config changed it.
        SDL_SetHintWithPriority("SDL_JOYSTICK_HIDAPI_PS5", "1",
                                SDL_HINT_DEFAULT);
        // Bluetooth DualSense controllers start in basic-report mode.
        // Enhanced reports carry gyro/accelerometer data (and rumble), so
        // request them before initializing the controller subsystem.
        SDL_SetHintWithPriority("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1",
                                SDL_HINT_DEFAULT);
        if (SDL_InitSubSystem(controller_flags) != 0) {
            std::fprintf(stderr,
                         "host_window: SDL controller/sensor init failed: %s\n",
                         SDL_GetError());
        }
    }

    auto* b = new Backend{};
    b->base_w = base_w;
    b->base_h = base_h;
    b->expanded_view = base_w != 240 || base_h != 160;
    b->resize_driven_view = resize_driven_view;
    b->freely_resizable_window = freely_resizable_window;
    b->linear_filter = linear_filter && !sharp_filter;
    b->sharp_filter = sharp_filter;
    b->scale = scale;
    b->title = title ? title : "gbarecomp";
#if defined(__ANDROID__)
    b->touch_controls = true;
#else
    if (const char* touch_env = std::getenv("GBARECOMP_TOUCH_CONTROLS"))
        b->touch_controls = *touch_env && *touch_env != '0';
#endif
    std::memcpy(b->bind_sc, kDefaultBinds, sizeof(kDefaultBinds));
    for (int h = 0; h < HK_COUNT; ++h)
        b->hotkeys[h] = parse_hotkey(kHotkeyDefaults[h]);
    // Linear vs nearest scaling is a texture-creation-time hint.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, linear_filter ? "linear" : "nearest");
    const int win_w = base_w * scale;
    const int win_h = base_h * scale;
    Uint32 window_flags = SDL_WINDOW_SHOWN |
        ((b->expanded_view || b->resize_driven_view ||
          b->freely_resizable_window)
             ? static_cast<Uint32>(SDL_WINDOW_RESIZABLE) : Uint32{0});
#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS;
#endif
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
#if defined(__ANDROID__)
    b->fullscreen = 1;
#endif
    // FramePacer is the sole emulation clock at the GBA's native 59.7275 Hz.
    // Do not put a blocking monitor-VSync present in series with it by default:
    // on a 165 Hz display that consumed a measured 4 ms median / 11 ms p95 of
    // the frame budget, reducing a warm GBA workload to 47-49 FPS and forcing
    // the audio bridge to stretch continuously. DWM still composites ordinary
    // windowed presentation; users who prefer synchronized fullscreen output
    // can opt in with GBARECOMP_VSYNC=1. GBARECOMP_NO_VSYNC remains the final
    // override for existing launch scripts and A/B diagnostics.
#if defined(_WIN32)
    SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "direct3d11",
                            SDL_HINT_DEFAULT);
#endif
    const char* vsync_env = std::getenv("GBARECOMP_VSYNC");
    const char* no_vsync_env = std::getenv("GBARECOMP_NO_VSYNC");
    const bool vsync_requested =
        vsync_env && *vsync_env && *vsync_env != '0';
    const bool vsync_disabled =
        no_vsync_env && *no_vsync_env && *no_vsync_env != '0';
    const bool want_vsync =
        vsync_requested && !vsync_disabled;
    const Uint32 renderer_flags = SDL_RENDERER_ACCELERATED |
        (want_vsync ? static_cast<Uint32>(SDL_RENDERER_PRESENTVSYNC)
                    : Uint32{0});
    b->renderer = SDL_CreateRenderer(b->window, -1, renderer_flags);
    if (!b->renderer && want_vsync) {
        std::fprintf(stderr,
                     "host_window: synchronized renderer unavailable; "
                     "falling back to unsynchronized presentation: %s\n",
                     SDL_GetError());
        b->renderer = SDL_CreateRenderer(
            b->window, -1, SDL_RENDERER_ACCELERATED);
    }
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
    {
        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(b->renderer, &info) == 0) {
            std::fprintf(stderr,
                         "host_window: renderer=%s flags=0x%08x vsync=%s%s "
                         "scaler=%s\n",
                         info.name ? info.name : "unknown",
                         static_cast<unsigned>(info.flags),
                         (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "yes" : "no",
                         b->resize_driven_view ? " (adaptive)" : "",
                         b->sharp_filter ? "sharp"
                         : b->linear_filter ? "linear" : "nearest");
            std::fflush(stderr);
        }
        log_display_mode(b->window, "open");
    }
    b->cadence.init();
#if defined(__ANDROID__)
    // Mobile presentation owns the whole drawable: a centered integer-scaled
    // game viewport plus dedicated side rails for touch controls.
    SDL_RenderSetLogicalSize(b->renderer, 0, 0);
    SDL_SetRenderDrawColor(b->renderer, 7, 11, 20, 255);
#else
    if (b->expanded_view || b->resize_driven_view) {
        // The destination viewport is computed explicitly in present() so
        // resizing maximally fills the drawable at the selected widescreen
        // aspect. Exact multiples retain integer scale; filtering follows the
        // linear_filter choice set above (historical default: nearest).
        SDL_SetRenderDrawColor(b->renderer, 0, 0, 0, 255);
    } else {
        // Non-opting games retain the historical fixed 240x160 SDL logical
        // renderer, including its window flags and copy path.
        SDL_RenderSetLogicalSize(b->renderer, base_w, base_h);
    }
#endif

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
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(
        b->texture,
        b->linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif

#if defined(GBARECOMP_RUNTIME_UI)
    if (!runtime_imgui_init(b)) {
        SDL_DestroyTexture(b->texture);
        SDL_DestroyRenderer(b->renderer);
        SDL_DestroyWindow(b->window);
        delete b;
        return false;
    }
#endif

    open_first_game_controller(b);
    open_device_gyro(b);

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
    const char* direct_audio_env = std::getenv("GBARECOMP_AUDIO_DIRECT");
    b->audio_direct =
        direct_audio_env && *direct_audio_env && *direct_audio_env != '0';
    want.callback = b->audio_direct ? nullptr : gba_audio_callback;
    want.userdata = b->audio_direct ? nullptr : b;
    SDL_AudioSpec got{};
    // In direct mode, disallow application-visible format changes so SDL
    // converts the exact 65536 Hz mono stream to the physical device itself.
    // Bridge mode instead opens at the native rate and performs that clock
    // conversion in RAB.
    const char* audio_device_env = std::getenv("GBARECOMP_AUDIO_DEVICE");
    const char* audio_device_name =
        audio_device_env && *audio_device_env ? audio_device_env : nullptr;
    b->audio_dev = SDL_OpenAudioDevice(audio_device_name, /*iscapture=*/0,
                                       &want, &got,
                                       b->audio_direct
                                           ? 0
                                           : SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (b->audio_dev != 0) {
        if (b->audio_direct) {
            b->audio_direct_rate = got.freq;
            std::fprintf(stderr,
                         "host_window: audio=direct-queue driver=%s "
                         "device=%s format=%dHz mono S16 quantum=%u\n",
                         SDL_GetCurrentAudioDriver()
                             ? SDL_GetCurrentAudioDriver() : "(unknown)",
                         audio_device_name ? audio_device_name : "(default)",
                         got.freq, static_cast<unsigned>(got.samples));
            if (const char* probe = std::getenv("GBARECOMP_AUDIO_PROBE");
                probe && *probe && *probe != '0') {
                const int devices = SDL_GetNumAudioDevices(0);
                for (int i = 0; i < devices; ++i) {
                    std::fprintf(stderr,
                                 "host_window: audio_device[%d]=%s\n", i,
                                 SDL_GetAudioDeviceName(i, 0));
                }
            }
            std::fflush(stderr);
            // SDL devices start paused. push_audio_samples starts playback
            // after roughly four video frames have been queued.
        } else {
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
    if (b->audio_dev == 0) return;

    // Volume (launcher setting + VolumeUp/Down hotkeys): scale into scratch
    // before the bridge. 100 = passthrough, byte-identical to before.
    if (b->volume != 100) {
        b->volume_buf.resize(count);
        const int v = b->volume;
        for (std::size_t i = 0; i < count; ++i)
            b->volume_buf[i] = static_cast<int16_t>(
                (static_cast<int32_t>(samples[i]) * v) / 100);
        samples = b->volume_buf.data();
    }

    if (b->audio_direct) {
        const Uint32 queued_before = SDL_GetQueuedAudioSize(b->audio_dev);
        if (b->audio_direct_started && queued_before == 0) {
            ++b->audio_direct_underruns;
            // A reset, loading hitch, or other long producer stall exhausted
            // the queue. Pause and build a fresh cushion instead of repeatedly
            // restarting the device on one tiny block (audible as crackle).
            b->audio_direct_started = false;
            SDL_PauseAudioDevice(b->audio_dev, 1);
        }
        b->audio_direct_min_bytes =
            std::min(b->audio_direct_min_bytes, queued_before);

        const Uint32 bytes = static_cast<Uint32>(count * sizeof(int16_t));
        if (SDL_QueueAudio(b->audio_dev, samples, bytes) != 0) {
            static bool s_reported_queue_failure = false;
            if (!s_reported_queue_failure) {
                std::fprintf(stderr,
                             "host_window: SDL_QueueAudio failed: %s\n",
                             SDL_GetError());
                std::fflush(stderr);
                s_reported_queue_failure = true;
            }
            return;
        }
        const Uint32 queued_after = SDL_GetQueuedAudioSize(b->audio_dev);
        b->audio_direct_max_bytes =
            std::max(b->audio_direct_max_bytes, queued_after);
        ++b->audio_direct_pushes;
        b->audio_direct_samples += count;

        constexpr Uint32 kDirectPrerollBytes = 16384;  // 125 ms at 65536 Hz S16
        if (!b->audio_direct_started &&
            queued_after >= kDirectPrerollBytes) {
            b->audio_direct_started = true;
            SDL_PauseAudioDevice(b->audio_dev, 0);
        }

        static int s_direct_probe = -1;
        if (s_direct_probe < 0) {
            const char* e = std::getenv("GBARECOMP_AUDIO_PROBE");
            s_direct_probe = (e && *e && *e != '0') ? 1 : 0;
        }
        if (s_direct_probe) {
            for (std::size_t i = 0; i < count; ++i) {
                const int sample = samples[i];
                if (sample != 0) ++b->audio_direct_probe_nonzero;
                b->audio_direct_probe_peak =
                    std::max(b->audio_direct_probe_peak, std::abs(sample));
                b->audio_direct_probe_square_sum +=
                    static_cast<long double>(sample) * sample;
            }
            b->audio_direct_probe_samples += count;
        }
        if (s_direct_probe && (b->audio_direct_pushes % 120ULL) == 0ULL) {
            const double bytes_per_ms =
                static_cast<double>(b->audio_direct_rate) *
                sizeof(int16_t) / 1000.0;
            const double audio_secs =
                static_cast<double>(b->audio_direct_samples) / 65536.0;
            const double rms = b->audio_direct_probe_samples
                ? std::sqrt(static_cast<double>(
                      b->audio_direct_probe_square_sum /
                      b->audio_direct_probe_samples))
                : 0.0;
            std::fprintf(
                stderr,
                "[gba-audio-probe] direct pushes=%llu audio=%.1fs "
                "queue=%.1fms min=%.1fms max=%.1fms underrun=%llu "
                "block_nonzero=%zu/%zu rms=%.1f peak=%d\n",
                static_cast<unsigned long long>(b->audio_direct_pushes),
                audio_secs, queued_after / bytes_per_ms,
                b->audio_direct_min_bytes / bytes_per_ms,
                b->audio_direct_max_bytes / bytes_per_ms,
                static_cast<unsigned long long>(b->audio_direct_underruns),
                static_cast<std::size_t>(b->audio_direct_probe_nonzero),
                static_cast<std::size_t>(b->audio_direct_probe_samples),
                rms, b->audio_direct_probe_peak);
            std::fflush(stderr);
            b->audio_direct_probe_samples = 0;
            b->audio_direct_probe_nonzero = 0;
            b->audio_direct_probe_square_sum = 0.0;
            b->audio_direct_probe_peak = 0;
            b->audio_direct_min_bytes = queued_after;
            b->audio_direct_max_bytes = queued_after;
        }
        return;
    }
    if (!b->bridge_ready) return;

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
    b->cadence.dump();  // MC-WS-002: flush the cadence ring (verbose only)
    if (b->audio_dev) SDL_CloseAudioDevice(b->audio_dev);  // stops the callback first
    if (b->bridge_ready) rab_free(&b->bridge);
    if (b->audio_mtx) SDL_DestroyMutex(b->audio_mtx);
    close_game_controller(b);
    if (b->device_gyro) SDL_SensorClose(b->device_gyro);
#if defined(GBARECOMP_RUNTIME_UI)
    runtime_imgui_shutdown(b);
#endif
    destroy_sharp_texture(b);
    if (b->texture)   SDL_DestroyTexture(b->texture);
    if (b->renderer)  SDL_DestroyRenderer(b->renderer);
    if (b->window)    SDL_DestroyWindow(b->window);
    delete b;
    impl_ = nullptr;
    open_ = false;
}

bool HostWindow::set_surface_size(int base_w, int base_h) {
    if (!open_ || !impl_ || base_w < 1 || base_h < 1) return false;
    auto* b = static_cast<Backend*>(impl_);
    if (base_w == b->base_w && base_h == b->base_h) return true;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                b->linear_filter ? "linear" : "nearest");
    SDL_Texture* replacement = SDL_CreateTexture(
        b->renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        base_w, base_h);
    if (!replacement) {
        std::fprintf(stderr,
                     "host_window: dynamic SDL_CreateTexture failed: %s\n",
                     SDL_GetError());
        return false;
    }
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(
        replacement,
        b->linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif
    SDL_DestroyTexture(b->texture);
    destroy_sharp_texture(b);
    b->texture = replacement;
    b->base_w = base_w;
    b->base_h = base_h;
    b->expanded_view = base_w != 240 || base_h != 160;
    if (b->color_lut && !b->color_lut->is_passthrough())
        b->graded_fb.resize(static_cast<std::size_t>(base_w) * base_h * 3u);
    return true;
}

bool HostWindow::drawable_size(int* width, int* height) const {
    if (!open_ || !impl_ || !width || !height) return false;
    const auto* b = static_cast<const Backend*>(impl_);
    // Capture/debug override: GBARECOMP_FORCE_DRAWABLE="WxH" makes the
    // resize-driven view resolve as if the client window were WxH, so the
    // adaptive wide path can be exercised faithfully under the SDL dummy
    // video driver (headless, session-independent) for the MC-WS-002
    // frame-capture investigation. No effect unless set.
    static int s_fd_w = -1, s_fd_h = -1;
    if (s_fd_w == -1) {
        s_fd_w = 0;
        if (const char* e = std::getenv("GBARECOMP_FORCE_DRAWABLE")) {
            int w = 0, h = 0;
            if (std::sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                s_fd_w = w; s_fd_h = h;
            }
        }
    }
    if (s_fd_w > 0) { *width = s_fd_w; *height = s_fd_h; return true; }
    // The feature follows window aspect, not texture/renderer target size.
    // Some SDL backends keep RendererOutputSize pinned to the streaming
    // target while the client window is resized, so use the authoritative
    // live client extent first. HiDPI scaling is uniform and does not alter
    // the aspect ratio used by the policy.
    SDL_GetWindowSize(b->window, width, height);
    if (*width > 0 && *height > 0) return true;
    return SDL_GetRendererOutputSize(b->renderer, width, height) == 0 &&
           *width > 0 && *height > 0;
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
    SDL_SetRenderDrawColor(b->renderer, 7, 11, 20, 255);
    SDL_RenderClear(b->renderer);
#if defined(__ANDROID__)
    int drawable_w = 0;
    int drawable_h = 0;
    if (SDL_GetRendererOutputSize(
            b->renderer, &drawable_w, &drawable_h) != 0 ||
        drawable_w <= 0 || drawable_h <= 0) {
        SDL_GetWindowSize(b->window, &drawable_w, &drawable_h);
    }
    // Reserve roughly one fifth of the display on each side. Prefer an exact
    // integer multiple so pixel art stays crisp and symmetric.
    const int width_limited_scale =
        static_cast<int>((drawable_w * 0.62f) / b->base_w);
    const int height_limited_scale =
        static_cast<int>((drawable_h * 0.90f) / b->base_h);
    const int integer_scale =
        std::max(1, std::min(width_limited_scale, height_limited_scale));
    int game_w = b->base_w * integer_scale;
    int game_h = b->base_h * integer_scale;
    if (game_w > drawable_w || game_h > drawable_h) {
        const float scale = std::min(
            static_cast<float>(drawable_w) / b->base_w,
            static_cast<float>(drawable_h) / b->base_h);
        game_w = std::max(1, static_cast<int>(b->base_w * scale));
        game_h = std::max(1, static_cast<int>(b->base_h * scale));
    }
    const SDL_Rect destination{
        (drawable_w - game_w) / 2,
        (drawable_h - game_h) / 2,
        game_w,
        game_h};
    SDL_RenderCopy(b->renderer, b->texture, nullptr, &destination);
#else
    if (!b->expanded_view && !b->resize_driven_view) {
        SDL_RenderCopy(b->renderer, b->texture, nullptr, nullptr);
    } else {
        int drawable_w = 0;
        int drawable_h = 0;
        // Preserve the established renderer-output path for fixed-width
        // extended views (including MMZ). Resize-driven view uses the same
        // live client dimensions that selected its logical width, so the
        // texture and destination cannot disagree on backends whose renderer
        // output stays pinned to the original streaming target.
        if (b->resize_driven_view) {
            SDL_GetWindowSize(b->window, &drawable_w, &drawable_h);
        } else if (SDL_GetRendererOutputSize(
                       b->renderer, &drawable_w, &drawable_h) != 0) {
            SDL_GetWindowSize(b->window, &drawable_w, &drawable_h);
        }
        const PresentationLayout layout = compute_presentation_layout(
            drawable_w, drawable_h, b->base_w, b->base_h);
        if (layout.width > 0 && layout.height > 0) {
            const SDL_Rect destination = {
                layout.x, layout.y, layout.width, layout.height};
            bool sharp_presented = false;
            const int sharp_factor = b->sharp_filter
                ? compute_sharp_prescale_factor(
                      layout, b->base_w, b->base_h)
                : 0;
            if (sharp_factor > 0 &&
                ensure_sharp_texture(b, sharp_factor) &&
                SDL_SetRenderTarget(b->renderer, b->sharp_texture) == 0) {
                const bool prescaled =
                    SDL_RenderCopy(
                        b->renderer, b->texture, nullptr, nullptr) == 0;
                const bool restored =
                    SDL_SetRenderTarget(b->renderer, nullptr) == 0;
                if (prescaled && restored) {
                    sharp_presented =
                        SDL_RenderCopy(
                            b->renderer, b->sharp_texture, nullptr,
                            &destination) == 0;
                }
            }
            if (!sharp_presented) {
                // Exact integer scales and unsupported render-target backends
                // retain the crisp nearest path.
                SDL_SetRenderTarget(b->renderer, nullptr);
                SDL_RenderCopy(
                    b->renderer, b->texture, nullptr, &destination);
            }
        }
    }
#endif
    render_touch_controls(b);
#if defined(GBARECOMP_RUNTIME_UI)
    runtime_imgui_render(b);
#endif
    // MC-WS-002: time the present itself (vsync blocks here — or doesn't)
    // and stamp the DWM refresh counter into the always-on cadence ring.
    const uint64_t cad_qpc0 = SDL_GetPerformanceCounter();
    SDL_RenderPresent(b->renderer);
    b->cadence.record(cad_qpc0, SDL_GetPerformanceCounter(), b->fullscreen);

    // FPS readout (DisplayPerf hotkey): presents/sec, refreshed twice a
    // second in the title bar; the base title is restored when toggled off.
    if (b->fps_readout) {
        ++b->fps_presents;
        const Uint32 now = SDL_GetTicks();
        if (b->fps_window_start == 0) b->fps_window_start = now;
        const Uint32 span = now - b->fps_window_start;
        if (span >= 500) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s — %.1f fps", b->title.c_str(),
                          b->fps_presents * 1000.0 / span);
            SDL_SetWindowTitle(b->window, buf);
            b->fps_window_start = now;
            b->fps_presents = 0;
        }
    }
}

void HostWindow::load_input_config(const char* dir,
                                   bool assist_tools_default,
                                   int fast_forward_multiplier_default) {
    if (!open_ || !impl_ || !dir) return;
    auto* b = static_cast<Backend*>(impl_);
    b->assist_tools = assist_tools_default;
    b->assist_fast_forward_multiplier = std::clamp(
        fast_forward_multiplier_default, 2, 10);
    const std::string base = std::string(dir) + "/";

    // keybinds.ini [player1] (recomp-ui generic format, scancode names).
    ini_scan_section((base + "keybinds.ini").c_str(), "player1",
                     [b](const char* key, const char* val) {
        for (const auto& bk : kBindKeys) {
            if (SDL_strcasecmp(key, bk.name) != 0) continue;
            SDL_Scancode sc = scancode_from_name(val);
            b->bind_sc[bk.bit] = sc;   // "None"/unknown => unbound (UNKNOWN)
            return;
        }
    });

    // config.ini [KeyMap] (keycode names with Ctrl+/Alt+/Shift+ prefixes).
    ini_scan_section((base + "config.ini").c_str(), "KeyMap",
                     [b](const char* key, const char* val) {
        for (int h = 0; h < HK_COUNT; ++h) {
            if (SDL_strcasecmp(key, kHotkeyNames[h]) != 0) continue;
            b->hotkeys[h] = parse_hotkey(val);
            return;
        }
    });

    // Launcher-owned global Assist bindings. Numeric keyboard values are SDL
    // scancodes; controller values use recomp-ui's portable button/axis
    // encoding. Keeping these in [Launcher] lets the pre-boot menu and the
    // runtime share one configuration file.
    ini_scan_section((base + "config.ini").c_str(), "Launcher",
                     [b](const char* key, const char* val) {
        if (SDL_strcasecmp(key, "assist_tools") == 0)
            b->assist_tools = std::atoi(val) != 0;
        else if (SDL_strcasecmp(key, "assist_fast_forward_multiplier") == 0)
            b->assist_fast_forward_multiplier = std::clamp(
                std::atoi(val), 2, 10);
        else if (SDL_strcasecmp(key, "assist_rewind_key") == 0)
            b->assist_key[0] = static_cast<SDL_Scancode>(std::atoi(val));
        else if (SDL_strcasecmp(key, "assist_fast_key") == 0)
            b->assist_key[1] = static_cast<SDL_Scancode>(std::atoi(val));
        else if (SDL_strcasecmp(key, "assist_rewind_pad") == 0)
            b->assist_pad[0] = std::atoi(val);
        else if (SDL_strcasecmp(key, "assist_fast_pad") == 0)
            b->assist_pad[1] = std::atoi(val);
    });
}

bool HostWindow::assist_tools_enabled() const {
    if (!open_ || !impl_) return true;
    return static_cast<const Backend*>(impl_)->assist_tools;
}

int HostWindow::fast_forward_multiplier() const {
    if (!open_ || !impl_) return 4;
    return static_cast<const Backend*>(impl_)
        ->assist_fast_forward_multiplier;
}

void HostWindow::set_fullscreen(int mode) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
#if defined(__ANDROID__)
    (void)mode;
    b->fullscreen = 1;
    SDL_SetWindowFullscreen(b->window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    return;
#endif
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    if (b->fullscreen == mode) return;
    // 0 windowed, 1 borderless desktop, 2 exclusive fullscreen.
    Uint32 flag = mode == 2 ? SDL_WINDOW_FULLSCREEN
                : mode == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP
                            : 0;
    if (SDL_SetWindowFullscreen(b->window, flag) == 0) {
        b->fullscreen = mode;
        // Record which panel/mode the cadence data now runs on.
        log_display_mode(b->window,
                         mode == 2 ? "fullscreen-exclusive"
                         : mode == 1 ? "fullscreen-borderless"
                                     : "windowed");
    }
}

int HostWindow::fullscreen() const {
    if (!open_ || !impl_) return 0;
    return static_cast<const Backend*>(impl_)->fullscreen;
}

void HostWindow::adjust_scale(int delta) {
    if (!open_ || !impl_) return;
#if defined(__ANDROID__)
    (void)delta;
    return;
#endif
    auto* b = static_cast<Backend*>(impl_);
    if (b->fullscreen) return;   // meaningless while fullscreen
    int s = b->scale + delta;
    if (s < 1) s = 1;
    if (s > 8) s = 8;
    if (s == b->scale) return;
    b->scale = s;
    SDL_SetWindowSize(b->window, b->base_w * s, b->base_h * s);
    SDL_SetWindowPosition(b->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void HostWindow::set_volume(int pct) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    b->volume = pct;
}

int HostWindow::volume() const {
    if (!open_ || !impl_) return 100;
    return static_cast<const Backend*>(impl_)->volume;
}

int HostWindow::window_scale() const {
    if (!open_ || !impl_) return 1;
    return static_cast<const Backend*>(impl_)->scale;
}

void HostWindow::set_linear_filter(bool enabled) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (enabled && b->sharp_filter) {
        b->sharp_filter = false;
        destroy_sharp_texture(b);
    }
    b->linear_filter = enabled;
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(b->texture,
        enabled ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif
}

bool HostWindow::linear_filter() const {
    return open_ && impl_ && static_cast<const Backend*>(impl_)->linear_filter;
}

void HostWindow::set_audio_enabled(bool enabled) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->audio_dev) SDL_PauseAudioDevice(b->audio_dev, enabled ? 0 : 1);
}

bool HostWindow::audio_enabled() const {
    if (!open_ || !impl_) return false;
    auto* b = static_cast<const Backend*>(impl_);
    return b->audio_dev && SDL_GetAudioDeviceStatus(b->audio_dev) == SDL_AUDIO_PLAYING;
}

void HostWindow::set_resize_driven_view(bool enabled) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    b->resize_driven_view = enabled;
    SDL_SetWindowResizable(
        b->window,
        enabled || b->expanded_view || b->freely_resizable_window
            ? SDL_TRUE : SDL_FALSE);
}

#if defined(GBARECOMP_RUNTIME_UI)
void HostWindow::set_runtime_ui(RecompRuntimeUi* ui) {
    if (!open_ || !impl_) return;
    static_cast<Backend*>(impl_)->runtime_ui = ui;
}
#endif

void HostWindow::set_fps_readout(bool on) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->fps_readout == on) return;
    b->fps_readout = on;
    b->fps_window_start = 0;
    b->fps_presents = 0;
    if (!on) SDL_SetWindowTitle(b->window, b->title.c_str());
}

bool HostWindow::fps_readout() const {
    if (!open_ || !impl_) return false;
    return static_cast<const Backend*>(impl_)->fps_readout;
}

void HostWindow::service_events() {
    if (!open_ || !impl_) return;
    SDL_PumpEvents();
}

HostWindow::Events HostWindow::pump() {
    Events ev{};
    if (!open_) { ev.quit = true; return ev; }
    auto* b = static_cast<Backend*>(impl_);

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
#if defined(GBARECOMP_RUNTIME_UI)
        const bool finger_event =
            e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION ||
            e.type == SDL_FINGERUP;
        const bool mouse_event =
            e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN ||
            e.type == SDL_MOUSEBUTTONUP;
        const Uint32 event_now = SDL_GetTicks();
        const bool suppress_opening_touch =
            b->runtime_ui_suppressed_touch_releases > 0 ||
            (b->runtime_ui_suppress_touch_until != 0 &&
             !SDL_TICKS_PASSED(event_now,
                               b->runtime_ui_suppress_touch_until));
        if (finger_event && suppress_opening_touch) {
            if (e.type == SDL_FINGERDOWN) {
                ++b->runtime_ui_suppressed_touch_releases;
            } else if (e.type == SDL_FINGERUP) {
                --b->runtime_ui_suppressed_touch_releases;
                if (b->runtime_ui_suppressed_touch_releases <= 0) {
                    b->runtime_ui_suppressed_touch_releases = 0;
                    // SDL may follow FINGERUP with a synthetic mouse release.
                    b->runtime_ui_suppress_touch_until = event_now + 250;
                }
            }
            continue;
        }
        if (mouse_event && suppress_opening_touch) continue;
        if (b->runtime_imgui_ready) {
            ImGui::SetCurrentContext(b->runtime_imgui_context);
            ImGui_ImplSDL2_ProcessEvent(&e);
        }
        if (runtime_ui_event(b->runtime_ui, e)) continue;
#endif
        if (e.type == SDL_QUIT) {
            ev.quit = true;
        } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
            if (!b->controller) open_game_controller(b, e.cdevice.which);
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (b->controller &&
                e.cdevice.which == b->controller_id) {
                close_game_controller(b);
                open_first_game_controller(b);
            }
        } else if (e.type == SDL_WINDOWEVENT &&
                   e.window.event == SDL_WINDOWEVENT_CLOSE) {
            ev.quit = true;
        } else if (e.type == SDL_FINGERDOWN ||
                   e.type == SDL_FINGERMOTION ||
                   e.type == SDL_FINGERUP) {
            if (!b->touch_controls) continue;
#if defined(GBARECOMP_RUNTIME_UI)
            if (b->runtime_ui && recomp_runtime_ui_is_open(b->runtime_ui)) {
                if (e.type == SDL_FINGERUP) {
                    if (auto* touch = find_touch(b, e.tfinger.fingerId))
                        *touch = {};
                }
                continue;
            }
#endif
            if (e.type == SDL_FINGERDOWN) {
                if (auto* touch = allocate_touch(b, e.tfinger.fingerId)) {
                    touch->x = touch->start_x = e.tfinger.x;
                    touch->y = touch->start_y = e.tfinger.y;
                    touch->started_at = SDL_GetTicks();
                    touch->buttons = touch_buttons_at(touch->x, touch->y);
                    touch->long_press_candidate = touch->buttons == 0;
                }
            } else if (auto* touch = find_touch(b, e.tfinger.fingerId)) {
                touch->x = e.tfinger.x;
                touch->y = e.tfinger.y;
                touch->buttons = touch_buttons_at(touch->x, touch->y);
                const float dx = touch->x - touch->start_x;
                const float dy = touch->y - touch->start_y;
                if (touch->buttons != 0 || dx * dx + dy * dy > 0.000625f)
                    touch->long_press_candidate = false;
                if (e.type == SDL_FINGERUP) *touch = {};
            }
        } else if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
            // Edge-triggered hotkeys (ignore key-repeat). F1..F9 are
            // save-state slots: plain = load, Shift = save. SDL's F1..F12
            // keycodes are contiguous, so slot = sym - F1 + 1.
            SDL_Keycode sym = e.key.keysym.sym;
            Uint16 mods = e.key.keysym.mod;
            if (sym == SDLK_ESCAPE) {
                ev.quit = true;
            } else if (sym >= SDLK_F1 && sym <= SDLK_F9) {
                int slot = static_cast<int>(sym - SDLK_F1) + 1;
                if (mods & KMOD_SHIFT) ev.save_slot = slot;
                else                   ev.load_slot = slot;
            } else {
                // Rebindable system hotkeys (config.ini [KeyMap]).
                for (int h = 0; h < HK_COUNT; ++h) {
                    const HotkeyBind& hb = b->hotkeys[h];
                    if (hb.key == SDLK_UNKNOWN || hb.key != sym ||
                        !hotkey_mods_ok(hb, mods))
                        continue;
                    switch (h) {
                        case HK_FULLSCREEN:     ev.toggle_fullscreen = true; break;
                        case HK_PAUSE:          ev.toggle_pause = true;      break;
                        case HK_TURBO:          /* level-triggered below */  break;
                        case HK_WINDOW_BIGGER:  ev.window_bigger = true;     break;
                        case HK_WINDOW_SMALLER: ev.window_smaller = true;    break;
                        case HK_VOLUME_UP:      ev.volume_up = true;         break;
                        case HK_VOLUME_DOWN:    ev.volume_down = true;       break;
                        case HK_DISPLAY_PERF:   ev.toggle_fps = true;        break;
                        case HK_SOLAR_BRIGHTER: ev.solar_brighter = true;    break;
                        case HK_SOLAR_DIMMER:   ev.solar_dimmer = true;      break;
                        case HK_SOLAR_LIVE:     ev.solar_live = true;        break;
                    }
                }
            }
        }
    }

#if defined(GBARECOMP_RUNTIME_UI)
    if (b->touch_controls && b->runtime_ui &&
        !recomp_runtime_ui_is_open(b->runtime_ui)) {
        const Uint32 now = SDL_GetTicks();
        for (auto& touch : b->touches) {
            if (!touch.active || !touch.long_press_candidate ||
                now - touch.started_at < 650) {
                continue;
            }
            int active_fingers = 0;
            for (const auto& active_touch : b->touches)
                if (active_touch.active) ++active_fingers;
            recomp_runtime_ui_open(b->runtime_ui);
            b->runtime_ui_suppressed_touch_releases =
                std::max(1, active_fingers);
            b->runtime_ui_suppress_touch_until = 0;
            if (b->runtime_imgui_ready) {
                ImGui::SetCurrentContext(b->runtime_imgui_context);
                ImGui::GetIO().ClearInputMouse();
            }
            clear_touches(b);
            break;
        }
    }
#endif

    // Build the GBA KEYINPUT value from current keyboard state via the
    // rebindable table (keybinds.ini; defaults in kDefaultBinds).
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    uint16_t keys = 0x03FFu;  // all released
    for (int bit = 0; bit < 10; ++bit) {
        SDL_Scancode sc = b->bind_sc[bit];
        if (sc != SDL_SCANCODE_UNKNOWN && ks[sc])
            keys &= static_cast<uint16_t>(~(1u << bit));
    }

    // SDL's standard controller vocabulary maps naturally to the ten GBA
    // inputs. Keyboard and controller are additive so either can be used at
    // any time, including while testing motion.
    if (b->controller && SDL_GameControllerGetAttached(b->controller)) {
        const struct {
            int bit;
            SDL_GameControllerButton button;
        } pad_map[] = {
            {0, SDL_CONTROLLER_BUTTON_A},
            {1, SDL_CONTROLLER_BUTTON_B},
            {2, SDL_CONTROLLER_BUTTON_BACK},
            {3, SDL_CONTROLLER_BUTTON_START},
            {4, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
            {5, SDL_CONTROLLER_BUTTON_DPAD_LEFT},
            {6, SDL_CONTROLLER_BUTTON_DPAD_UP},
            {7, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
            {8, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
            {9, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
        };
        for (const auto& binding : pad_map) {
            if (SDL_GameControllerGetButton(b->controller, binding.button))
                keys &= static_cast<uint16_t>(~(1u << binding.bit));
        }
    }
    if (b->touch_controls) {
        const uint16_t touch_buttons = active_touch_buttons(b);
        keys &= static_cast<uint16_t>(~touch_buttons);
    }
    ev.keyinput = keys;
#if defined(GBARECOMP_RUNTIME_UI)
    if (b->runtime_ui && recomp_runtime_ui_is_open(b->runtime_ui))
        ev.keyinput = 0x03FFu;
#endif

    int mouse_x = 0;
    int mouse_y = 0;
    const Uint32 mouse_buttons = SDL_GetRelativeMouseState(&mouse_x, &mouse_y);
    ev.mouse_gyro_active =
        !b->touch_controls && (mouse_buttons & SDL_BUTTON_LMASK) != 0;
    ev.gyro_delta_x = ev.mouse_gyro_active ? mouse_x : 0;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (b->device_gyro) {
#if defined(__ANDROID__)
        // SDL normally updates sensors from SDL_PumpEvents(), but Android's
        // native sensor queue can remain unread after an Activity transition
        // even though SDL_SensorOpen() succeeded and SensorService registered
        // the client. Poll the sensor backend at the point of use so the value
        // below cannot depend on unrelated window/touch event traffic.
        SDL_SensorUpdate();
#endif
        float rate[3] = {};
        if (SDL_SensorGetData(b->device_gyro, rate, 3) == 0) {
            constexpr float kDeviceDriftDeadzone = 0.025f;
            ev.gyro_rate_z =
                std::abs(rate[2]) >= kDeviceDriftDeadzone ? rate[2] : 0.0f;
            if (ev.gyro_rate_z != 0.0f &&
                !b->device_gyro_motion_logged) {
                std::fprintf(stderr,
                             "host_window: device gyro input active "
                             "z=%.3f rad/s\n",
                             static_cast<double>(ev.gyro_rate_z));
                std::fflush(stderr);
                b->device_gyro_motion_logged = true;
            }
        }
    }
    if (b->controller && b->controller_gyro) {
        float rate[3] = {};
        if (SDL_GameControllerGetSensorData(
                b->controller, SDL_SENSOR_GYRO, rate, 3) == 0) {
            // SDL reports radians/second. Z is rotation around the controller
            // face normal: the steering-wheel-like twist WarioWare expects.
            constexpr float kDriftDeadzone = 0.035f;
            ev.gyro_rate_z =
                std::abs(rate[2]) >= kDriftDeadzone ? rate[2] : 0.0f;
        }
    }
#endif

    // Turbo is level-triggered: held = uncap the frame limiter. The historical
    // [KeyMap] Turbo binding (default Tab) remains as a compatibility shortcut;
    // the launcher-editable Assist binding is additive.
    ev.fast_forward = false;
    {
        const HotkeyBind& hb = b->hotkeys[HK_TURBO];
        if (hb.key != SDLK_UNKNOWN) {
            SDL_Scancode sc = SDL_GetScancodeFromKey(hb.key);
            if (sc != SDL_SCANCODE_UNKNOWN && ks[sc] &&
                hotkey_mods_ok(hb, SDL_GetModState()))
                ev.fast_forward = true;
        }
    }
    // Always report the configured physical controls. The runtime owns the
    // Assist gate, which lets its in-game menu enable or disable these actions
    // immediately without HostWindow carrying a stale copy of that live state.
    const SDL_Scancode fast_sc = b->assist_key[1];
    if ((fast_sc > SDL_SCANCODE_UNKNOWN && fast_sc < SDL_NUM_SCANCODES &&
         ks[fast_sc]) || assist_pad_down(b->controller, b->assist_pad[1]))
        ev.fast_forward = true;

    const SDL_Scancode rewind_sc = b->assist_key[0];
    const bool rewind_down =
        (rewind_sc > SDL_SCANCODE_UNKNOWN && rewind_sc < SDL_NUM_SCANCODES &&
         ks[rewind_sc]) || assist_pad_down(b->controller, b->assist_pad[0]);
    const Uint32 rewind_now = SDL_GetTicks();
    ev.rewind = rewind_down &&
        (!b->assist_rewind_was_down ||
         SDL_TICKS_PASSED(rewind_now, b->assist_rewind_repeat_at));
    if (ev.rewind) b->assist_rewind_repeat_at = rewind_now + 500;
    if (!rewind_down) b->assist_rewind_repeat_at = 0;
    b->assist_rewind_was_down = rewind_down;
    return ev;
}

}  // namespace gbarecomp

#else  // !GBARECOMP_HAVE_SDL2 — stub backend

namespace gbarecomp {

HostWindow::HostWindow()  = default;
HostWindow::~HostWindow() = default;

bool HostWindow::is_available() { return false; }

bool HostWindow::open(int /*scale*/, int /*base_w*/, int /*base_h*/,
                      const char* /*title*/, const char* /*screen*/,
                      bool /*linear_filter*/, bool /*sharp_filter*/,
                      bool /*resize_driven_view*/,
                      bool /*freely_resizable_window*/) {
    std::fprintf(stderr,
                 "host_window: built without SDL2; --window unavailable\n");
    return false;
}

void HostWindow::close() { open_ = false; }

bool HostWindow::set_surface_size(int /*base_w*/, int /*base_h*/) {
    return false;
}

bool HostWindow::drawable_size(int* /*width*/, int* /*height*/) const {
    return false;
}

void HostWindow::present(const uint8_t* /*rgb888*/) {}

void HostWindow::load_input_config(const char* /*dir*/,
                                   bool /*assist_tools_default*/,
                                   int /*fast_forward_multiplier_default*/) {}
void HostWindow::set_fullscreen(int /*mode*/) {}
int  HostWindow::fullscreen() const { return 0; }
void HostWindow::adjust_scale(int /*delta*/) {}
void HostWindow::set_volume(int /*pct*/) {}
int  HostWindow::volume() const { return 100; }
int  HostWindow::window_scale() const { return 1; }
void HostWindow::set_linear_filter(bool /*enabled*/) {}
bool HostWindow::linear_filter() const { return false; }
void HostWindow::set_audio_enabled(bool /*enabled*/) {}
bool HostWindow::audio_enabled() const { return false; }
void HostWindow::set_resize_driven_view(bool /*enabled*/) {}
#if defined(GBARECOMP_RUNTIME_UI)
void HostWindow::set_runtime_ui(RecompRuntimeUi* /*ui*/) {}
#endif
void HostWindow::set_fps_readout(bool /*on*/) {}
bool HostWindow::fps_readout() const { return false; }
bool HostWindow::assist_tools_enabled() const { return true; }
int HostWindow::fast_forward_multiplier() const { return 4; }

void HostWindow::push_audio_samples(const int16_t* /*samples*/,
                                    std::size_t /*count*/) {}

void HostWindow::service_events() {}

HostWindow::Events HostWindow::pump() {
    Events ev{};
    ev.quit = true;
    return ev;
}

}  // namespace gbarecomp

#endif  // GBARECOMP_HAVE_SDL2
