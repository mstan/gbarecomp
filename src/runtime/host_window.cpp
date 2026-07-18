// host_window.cpp — SDL2-backed window. Stubs out when SDL2 isn't found.

#include "host_window.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
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
    HK_COUNT
};

struct HotkeyBind {
    SDL_Keycode key = SDLK_UNKNOWN;   // SDLK_UNKNOWN = unbound
    Uint16      mods = 0;             // required KMOD_CTRL/ALT/SHIFT bits
};

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
    bool resize_driven_view = false;
    bool linear_filter = false;

    // ---- rebindable input (see HostWindow::load_input_config) --------------
    // GBA KEYINPUT bit (0..9) per bound SDL scancode; seeded from the
    // built-in defaults, overridden by keybinds.ini [player1].
    SDL_Scancode bind_sc[10] = {};      // indexed by GBA KEYINPUT bit
    HotkeyBind   hotkeys[HK_COUNT];     // [KeyMap] bindings
    int          scale = 3;             // current integer window scale
    bool         fullscreen = false;
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
};
const char* const kHotkeyDefaults[HK_COUNT] = {
    "Alt+Return", "Shift+P", "Tab",
    "", "",
    "", "", "F",
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

}  // namespace

HostWindow::HostWindow() = default;

HostWindow::~HostWindow() {
    close();
}

bool HostWindow::is_available() { return true; }

bool HostWindow::open(int scale, int base_w, int base_h, const char* title,
                      const char* screen, bool linear_filter,
                      bool resize_driven_view) {
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
    b->resize_driven_view = resize_driven_view;
    b->linear_filter = linear_filter;
    b->scale = scale;
    b->title = title ? title : "gbarecomp";
    std::memcpy(b->bind_sc, kDefaultBinds, sizeof(kDefaultBinds));
    for (int h = 0; h < HK_COUNT; ++h)
        b->hotkeys[h] = parse_hotkey(kHotkeyDefaults[h]);
    // Linear vs nearest scaling is a texture-creation-time hint.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, linear_filter ? "linear" : "nearest");
    const int win_w = base_w * scale;
    const int win_h = base_h * scale;
    const Uint32 window_flags = SDL_WINDOW_SHOWN |
        ((b->expanded_view || b->resize_driven_view)
             ? static_cast<Uint32>(SDL_WINDOW_RESIZABLE) : Uint32{0});
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
    // The independent FramePacer still governs emulation at 59.7275 Hz
    // (MC-HP-004) — vsync below only aligns scanout, in series after the
    // pacer, and can never become the game clock.
    // HP-002: EVERY path now requests a synchronized present. The cadence
    // probe measured the legacy D3D9 blit returning in <1 ms despite
    // vsync=yes (presents never sync to scanout → the tear band users see
    // toward the bottom at native res). SDL2's D3D11 backend is a DXGI
    // flip-model swapchain whose vsync genuinely blocks, and windowed VRR
    // (G-Sync "windowed and full screen") can engage through it — so prefer
    // it by default on Windows; SDL_RENDER_DRIVER in the environment still
    // overrides. GBARECOMP_NO_VSYNC=1 restores the historical
    // unsynchronized present for A/B.
#if defined(_WIN32)
    SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "direct3d11",
                            SDL_HINT_DEFAULT);
#endif
    const char* no_vsync_env = std::getenv("GBARECOMP_NO_VSYNC");
    const bool want_vsync =
        !(no_vsync_env && *no_vsync_env && *no_vsync_env != '0');
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
                         "host_window: renderer=%s flags=0x%08x vsync=%s%s\n",
                         info.name ? info.name : "unknown",
                         static_cast<unsigned>(info.flags),
                         (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "yes" : "no",
                         b->resize_driven_view ? " (adaptive)" : "");
            std::fflush(stderr);
        }
        log_display_mode(b->window, "open");
    }
    b->cadence.init();
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
    SDL_DestroyTexture(b->texture);
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
    SDL_RenderClear(b->renderer);
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
            SDL_RenderCopy(b->renderer, b->texture, nullptr, &destination);
        }
    }
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

void HostWindow::load_input_config(const char* dir) {
    if (!open_ || !impl_ || !dir) return;
    auto* b = static_cast<Backend*>(impl_);
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
}

void HostWindow::set_fullscreen(bool on) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->fullscreen == on) return;
    if (SDL_SetWindowFullscreen(b->window,
                                on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0) {
        b->fullscreen = on;
        // Record which panel/mode the cadence data now runs on.
        log_display_mode(b->window, on ? "fullscreen" : "windowed");
    }
}

bool HostWindow::fullscreen() const {
    if (!open_ || !impl_) return false;
    return static_cast<const Backend*>(impl_)->fullscreen;
}

void HostWindow::adjust_scale(int delta) {
    if (!open_ || !impl_) return;
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

HostWindow::Events HostWindow::pump() {
    Events ev{};
    if (!open_) { ev.quit = true; return ev; }
    auto* b = static_cast<Backend*>(impl_);

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
                    }
                }
            }
        }
    }

    // Build the GBA KEYINPUT value from current keyboard state via the
    // rebindable table (keybinds.ini; defaults in kDefaultBinds).
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    uint16_t keys = 0x03FFu;  // all released
    for (int bit = 0; bit < 10; ++bit) {
        SDL_Scancode sc = b->bind_sc[bit];
        if (sc != SDL_SCANCODE_UNKNOWN && ks[sc])
            keys &= static_cast<uint16_t>(~(1u << bit));
    }
    ev.keyinput = keys;

    // Turbo is level-triggered: held = uncap the frame limiter (default Tab).
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
                      bool /*linear_filter*/, bool /*resize_driven_view*/) {
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

void HostWindow::load_input_config(const char* /*dir*/) {}
void HostWindow::set_fullscreen(bool /*on*/) {}
bool HostWindow::fullscreen() const { return false; }
void HostWindow::adjust_scale(int /*delta*/) {}
void HostWindow::set_volume(int /*pct*/) {}
int  HostWindow::volume() const { return 100; }
void HostWindow::set_fps_readout(bool /*on*/) {}
bool HostWindow::fps_readout() const { return false; }

void HostWindow::push_audio_samples(const int16_t* /*samples*/,
                                    std::size_t /*count*/) {}

HostWindow::Events HostWindow::pump() {
    Events ev{};
    ev.quit = true;
    return ev;
}

}  // namespace gbarecomp

#endif  // GBARECOMP_HAVE_SDL2
