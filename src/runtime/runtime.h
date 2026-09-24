// runtime.h — STUB. Glue that game binaries link against.
//
// Owns lifecycle: ROM load + hash verify, BIOS load, CPU + bus + PPU
// init, scheduler start, debug server start, main loop. The generated
// C from gba_recompile expects this header to provide the dispatch
// entry points and host-platform helpers it calls into.

#pragma once

#include <cstddef>
#include <cstdint>

namespace gbarecomp {

struct TouchFrameInfo;   // touch_input.h
class HostOverlay;       // host_overlay.h

// Read-only state published to an opted-in game's extended-view policy once
// per emulated frame. The shared runtime owns the wider surface; the game owns
// the decision about whether the current scene has authentic margin content.
// Keeping this view deliberately small avoids exposing mutable bus internals.
struct ExtendedViewFrameInfo {
    std::uint64_t frame_count = 0;
    std::uint32_t view_width = 240;
    std::uint32_t extra_left = 0;
    std::uint32_t extra_right = 0;
    std::uint32_t view_height = 160;
    std::uint32_t extra_top = 0;
    std::uint32_t extra_bottom = 0;
    const std::uint8_t* io = nullptr;
    std::size_t io_size = 0;
};

// Per-game built-in defaults baked into a game runner at compile time.
// Lets a standalone release .exe (e.g. MinishCapRecomp.exe) ship
// without a sibling game.toml — the runtime falls back to these
// values when no TOML is found and no CLI override is supplied.
//
// All fields are optional. A null pointer / 0 means "no built-in" and
// the runtime keeps whatever it would have used otherwise (BIOS
// constants from GbaBios, empty ROM hash that forces the user to
// provide --config or --rom-sha1).
struct RunOptions {
    const char*   builtin_game_name = nullptr;
    const char*   builtin_rom_sha1  = nullptr;
    std::uint32_t builtin_rom_crc32 = 0;

    // Non-null opts this game into the data-only .gbamod package catalog.
    // Packages target this stable ID plus builtin_rom_sha1 and may activate
    // only trusted callbacks statically registered by the game executable.
    const char* mod_game_id = nullptr;
    // Make a view feature authoritative over legacy TOML, CLI, and environment
    // inputs, including its fixed/initial width request. Games with unrelated
    // catalogs leave this false.
    bool mod_owns_adaptive_view = false;

    // Extended horizontal view is a game-owned enhancement capability, not a
    // generic emulator toggle. Values above the native 240 are the opt-in;
    // the default therefore makes stale config/environment settings inert.
    std::uint16_t max_view_width = 240;

    // A separate extended-view policy for games whose logical width follows
    // the live host-window aspect ratio. This ceiling is deliberately not
    // max_view_width: opting into resize-driven view does not also authorize
    // fixed --view-width modes. The launcher/CLI must explicitly opt in with
    // --resize-view; an accompanying --view-width may seed the initial
    // windowed aspect, while adaptive fullscreen follows the host display.
    std::uint16_t max_resize_view_width = 240;
    // Vertical margins require game-authored scenery/OBJ providers. This
    // independent opt-in leaves every existing game's height at 160.
    std::uint16_t max_resize_view_height = 160;
    bool resize_driven_view = false;

    // How a resize-driven view picks its logical size from the drawable.
    //   Aspect  — keep 160 lines, widen with the window aspect (desktop; the
    //             established behaviour).
    //   Density — keep a physical logical-pixel size (touch devices), growing
    //             BOTH axes on large screens (see density_driven_view_geometry).
    // --view-density / GBARECOMP_VIEW_DENSITY force Density for development.
    enum class ViewSizing : std::uint8_t { Aspect = 0, Density = 1 };
    ViewSizing resize_view_sizing = ViewSizing::Aspect;
    float density_mm_per_logical_px = 0.30f;

    // Per-present presentation request from the game's scene policy. Scenes
    // without authored margins (battles, full-screen menus) can ask for the
    // exact native 240x160 view so it is drawn as large as possible, and, in
    // portrait, anchored to the top of the screen so host-drawn controls get
    // the space below. Null keeps the resize-driven geometry for every scene.
    struct PresentationRequest {
        bool native_view = false;
        bool anchor_top = false;
    };
    void (*presentation_request)(PresentationRequest* request) = nullptr;

    // Resume the suspend state written when the OS backgrounded the app, if
    // the process was killed before returning to the foreground (mobile).
    bool resume_suspend_state_on_launch = false;

    // Device orientation policy on platforms that rotate (Android). Landscape
    // is the historical default; Any follows the sensor with live re-layout.
    enum class Orientation : std::uint8_t { Landscape = 0, Portrait = 1, Any = 2 };
    Orientation orientation = Orientation::Landscape;

    // Host-window presentation policy, independent of extended guest view.
    // When true the player may freely reshape a window while SDL keeps the
    // native game image aspect-correct with letter/pillar boxing.
    bool freely_resizable_window = false;

    // Player-facing diagnostics. The counter reports actual presents per
    // second in the native window title; games may ship it enabled while
    // retaining the shared hotkey/menu toggle.
    bool show_fps_by_default = false;

    // Optional shared Assist Tools surface. Games opt into the menu and choose
    // its resource bounds; the implementation remains engine-owned and
    // reusable. Slot hotkeys continue to cover 1-9, while menus can reach 10.
    bool expose_assist_tools = false;
    bool assist_tools_enabled_by_default = true;
    std::uint8_t assist_fast_forward_multiplier_default = 4;
    std::uint8_t save_state_slot_count = 9;
    std::uint8_t rewind_history_seconds = 0;
    std::uint8_t rewind_capture_interval_frames = 15;

    // Optional game-owned content initializer. Called exactly once, after the
    // first non-native view has been authorized and applied. For a fixed view
    // that is during startup; for resize-driven view it is deferred until the
    // host aspect first expands past 240. Keeping this null at 240 preserves
    // the generated function-entry fast path too.
    void (*extended_view_init)(std::uint32_t extra_left,
                               std::uint32_t extra_right) = nullptr;

    // Optional game-owned per-frame margin policy. It runs at the start of an
    // emulated frame, before scanline 0 is composed, and again immediately
    // after a live adaptive resize. Games may use the read-only PPU registers
    // above to select shared margin providers/pillarboxing. Null leaves the
    // established renderer behavior unchanged.
    void (*extended_view_frame)(const ExtendedViewFrameInfo* frame) = nullptr;

    // ---- game-owned touch input policy (touch_input.h) ----------------------
    // Called once per emulated frame at VBlank start — before the VBlank IRQ
    // is raised, so the guest's input read for that frame observes the
    // result — with the frame's touch events, recognized gestures and live
    // pointers. Returns an ACTIVE-LOW KEYINPUT mask that is ANDed with the
    // host keys (0x03FF = synthesize nothing). Runs on the guest thread, so
    // it may read guest memory, but it must only STEER through the returned
    // keys: guest state is never written by the policy. Null leaves the
    // established input path byte-identical. Suspended during input replay
    // (the recorded trace already contains the composed keys).
    std::uint16_t (*input_frame)(const TouchFrameInfo* frame) = nullptr;

    // Gestures the game's policy owns (TouchClaim bits). An unclaimed
    // long-press still opens the runtime settings menu; a three-finger tap
    // always does.
    std::uint32_t touch_gesture_claims = 0;

    // Virtual gamepad overlay default when config.ini has no saved choice:
    // -1 = platform default (on for Android, off on desktop), 0 = off, 1 = on.
    // The player toggles it with the corner button; the choice persists.
    int touch_pad_default = -1;

    // Optional host-drawn presentation layer (host_overlay.h). Called on every
    // present after the game image, before the settings menu; guest thread.
    void (*host_overlay)(HostOverlay* overlay) = nullptr;

    // Optional game-owned debug TCP commands (JSON line in, JSON line out).
    // Return non-zero when the request was handled and `reply` was written
    // through `write` (may be called repeatedly to stream a large reply).
    int (*tcp_command)(const char* request,
                       void (*write)(void* ctx, const char* data, std::size_t len),
                       void* write_ctx) = nullptr;

    // This cartridge carries a solar sensor. Unlike the RTC there is no ROM
    // signature to detect one from, so it has to be declared. Games that leave
    // this false are unaffected; GBARECOMP_SOLAR still forces it on for
    // experiments, and RECOMP_SOLAR_OFF forces it off regardless.
    bool has_solar_sensor = false;

    // Optional game-owned light source for the cartridge solar sensor
    // (gba_solar.h). Emulating the sensor is a runner CAPABILITY; deciding
    // where light comes from is game POLICY, so anything with I/O — a camera,
    // a weather service — belongs in the game binary behind this seam and not
    // in the engine. Returns host brightness, 0 = dark, 255 = full sun.
    //
    // Sampled once per ADC conversion on the guest's thread, so it MUST be
    // non-blocking: a network-backed provider polls on its own thread and
    // answers from cache. Null leaves the sensor dark until a configured solar
    // hotkey is pressed, which is the pre-existing behaviour.
    //
    // SolarBrighter/SolarDimmer override this while set; SolarLive releases
    // the override. All three are unbound by default.
    std::uint8_t (*solar_provider)() = nullptr;

    // ---- game-owned items appended to the in-game settings menu -------------
    // The runtime builds the common surface (display, audio, save states) from
    // recomp-ui's standard catalog, which necessarily knows nothing about any
    // one cartridge. A game with its own player-facing settings — Boktai's
    // light source, for instance — contributes them here instead of teaching
    // the engine about them, so the only alternative to a menu entry is not an
    // undocumented keystroke.
    //
    // ui_extra_items points at an array of recomp-ui `RecompRuntimeUiItem`,
    // deliberately typed as void* so runtime.h stays parseable (and RunOptions
    // stays one layout) in builds compiled without the runtime-UI headers. The
    // array and every string it references must outlive run_game().
    //
    // Keys the engine does not recognize fall through to these callbacks, which
    // return non-zero when they handled the key. Called on the main loop's
    // thread between frames, never mid-frame.
    const void* ui_extra_items      = nullptr;
    std::size_t ui_extra_item_count = 0;
    int (*ui_get)(const char* key, int* value_out) = nullptr;
    int (*ui_set)(const char* key, int value)      = nullptr;
    int (*ui_action)(const char* key)              = nullptr;
    // Greys an item out. Unlike the three above this is a positive answer
    // (non-zero = selectable), so a game that only wants to disable a couple
    // of keys returns 1 for everything else. Null = everything enabled.
    int (*ui_enabled)(const char* key)             = nullptr;
    // Text-valued settings (recomp-ui's RECOMP_RUNTIME_UI_TEXT). ui_get_text
    // fills buf NUL-terminated; ui_set_text returns non-zero if it accepted the
    // value. A postal code or a server address is neither a number nor a fixed
    // choice, and stepping one with -/+ is unusable.
    int (*ui_get_text)(const char* key, char* buf, std::size_t buf_len) = nullptr;
    int (*ui_set_text)(const char* key, const char* value) = nullptr;

    // Requests recomp-ui's physically-small/high-density touch presentation:
    // near-full-screen panels, larger hit targets, and touch-oriented footer
    // copy. False preserves the desktop/TV presentation for existing games.
    bool ui_touch_friendly = false;

    // Game/device calibration beneath the user-facing gyro multiplier.
    // A menu value of 1.00x means this game's authored baseline; 0.75 here
    // makes that baseline 75% of the engine's raw sensor conversion while the
    // player-facing value remains centered on 1.00x.
    float gyro_sensitivity_calibration = 1.0f;

    // ---- pre-boot launcher identity (launcher_seam.h, RECOMP_LAUNCHER builds) --
    // Consumed by the recomp-ui launcher seam a game's main() runs BEFORE
    // run_game(); the runtime itself never reads these. All optional.
    const char* launcher_region = nullptr;      // display region, e.g. "USA"
    // The game's default game.toml path (GBARECOMP_DEFAULT_GAME_CONFIG). The
    // seam reads its [rom].path / [bios].path to PREFILL the launcher when no
    // rom.cfg / bios.cfg sidecar exists yet, so a first run isn't blank.
    const char* launcher_game_config = nullptr;
    // Optional launcher state/cache filenames, relative to the executable dir.
    // Multi-variant repos can use these to keep side-by-side launchers from
    // sharing one config.ini / rom.cfg in the same build output directory.
    const char* launcher_config_filename = nullptr;
    const char* launcher_keybinds_filename = nullptr;
    const char* launcher_rom_cache_filename = nullptr;
    const char* launcher_bios_cache_filename = nullptr;
    const char* launcher_save_path = nullptr;   // explicit save file (game.toml
                                                // [save].path); null => <rom>.sav
                                                // derived from the seeded ROM
    // Opt into recomp-ui's verified IPS/IPS32/BPS source-ROM patch surface.
    // Patched images are generated under the executable's mods/rom-patches
    // cache; the original ROM and patch file are never modified.
    bool launcher_enable_rom_patches = false;
    const char* launcher_rom_patch_note = nullptr;
    // Translation-specific builds can verify a stock source independently
    // from builtin_rom_sha1 (the target image the generated corpus expects).
    const char* launcher_source_rom_sha1 = nullptr;
    // Lowercase SHA-1 of the only accepted patched output for a corpus built
    // against one translation release. Null leaves data-only patches open.
    const char* launcher_required_patch_sha1 = nullptr;
    // Keep an implemented extended-view mode out of the public launcher while
    // it is still being profiled. Explicit CLI/TOML opt-ins remain available.
    // Defaults true so existing games (including MMZ) retain today's UI.
    bool launcher_expose_widescreen = true;
    // Show recomp-ui's Adaptive view toggle for this game. Runtime support
    // (resize_driven_view + max_resize_view_width) is necessary but not
    // sufficient: every title must explicitly opt into the launcher surface
    // after its live-resize presentation has been validated.
    bool launcher_expose_adaptive_view = false;
    // Show recomp-ui's controller-motion sensitivity surface. The runtime
    // remains responsible for discovering a sensor and mapping its angular
    // rate onto the cartridge peripheral.
    bool launcher_expose_gyro = false;
    // Optional renderer enhancements exposed as independent recomp-ui Video
    // toggles. Defaults are explicit per game so an older config.ini that
    // lacks the new keys inherits the title's intended shipped behavior.
    bool launcher_expose_sharp_filter = false;
    bool launcher_default_sharp_filter = false;
    bool launcher_expose_affine_filter = false;
    bool launcher_default_affine_filter = false;
    // >240 offers the launcher's 16:9 widescreen toggle, mapped to
    // --view-width <this> when enabled. 0/240 = no widescreen surface shown.
    // Games with MULTIPLE extended widths use the aspect vocabulary below
    // instead (takes precedence when set).
    std::uint16_t widescreen_view_width = 0;
    // Game-supplied aspect vocabulary for the launcher's aspect cycle
    // (EXPERIMENTAL-tagged). labels/view_widths are parallel arrays of
    // num_aspects entries; index 0 must be the native 240 view. The
    // committed index maps to --view-width <view_widths[index]>.
    // e.g. Mega Man Zero: {"3:2 (Native)","9:5 (288 px)","12:5 (384 px)",
    // "6:2 (480 px)"} / {240, 288, 384, 480}.
    const char* const*   launcher_aspect_labels = nullptr;
    const std::uint16_t* launcher_aspect_view_widths = nullptr;
    int                  launcher_num_aspects = 0;
    // Box-art image path relative to the assets dir staged next to the exe;
    // null => the launcher's default "assets/img/boxart.tga". Multi-variant
    // repos stage one file per variant (e.g. "assets/img/boxart_firered.tga").
    const char* launcher_boxart = nullptr;
};

int run_game(int argc, char** argv, const RunOptions& opts = {});

}  // namespace gbarecomp
