// runtime.h — STUB. Glue that game binaries link against.
//
// Owns lifecycle: ROM load + hash verify, BIOS load, CPU + bus + PPU
// init, scheduler start, debug server start, main loop. The generated
// C from gba_recompile expects this header to provide the dispatch
// entry points and host-platform helpers it calls into.

#pragma once

#include <cstdint>

namespace gbarecomp {

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

    // Extended horizontal view is a game-owned enhancement capability, not a
    // generic emulator toggle. Values above the native 240 are the opt-in;
    // the default therefore makes stale config/environment settings inert.
    std::uint16_t max_view_width = 240;

    // A separate extended-view policy for games whose logical width follows
    // the live host-window aspect ratio. This ceiling is deliberately not
    // max_view_width: opting into resize-driven view does not also authorize
    // fixed --view-width modes. The game starts at faithful 240x160, and the
    // launcher/CLI must explicitly opt in with --resize-view.
    std::uint16_t max_resize_view_width = 240;
    bool resize_driven_view = false;

    // Optional game-owned content initializer. Called exactly once, after the
    // first non-native view has been authorized and applied. For a fixed view
    // that is during startup; for resize-driven view it is deferred until the
    // host aspect first expands past 240. Keeping this null at 240 preserves
    // the generated function-entry fast path too.
    void (*extended_view_init)(std::uint32_t extra_left,
                               std::uint32_t extra_right) = nullptr;

    // ---- pre-boot launcher identity (launcher_seam.h, RECOMP_LAUNCHER builds) --
    // Consumed by the recomp-ui launcher seam a game's main() runs BEFORE
    // run_game(); the runtime itself never reads these. All optional.
    const char* launcher_region = nullptr;      // display region, e.g. "USA"
    // The game's default game.toml path (GBARECOMP_DEFAULT_GAME_CONFIG). The
    // seam reads its [rom].path / [bios].path to PREFILL the launcher when no
    // rom.cfg / bios.cfg sidecar exists yet, so a first run isn't blank.
    const char* launcher_game_config = nullptr;
    const char* launcher_save_path = nullptr;   // explicit save file (game.toml
                                                // [save].path); null => <rom>.sav
                                                // derived from the seeded ROM
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
