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

    // Optional game-owned content initializer. Called exactly once, and only
    // after a non-native view has been authorized and applied. Keeping this
    // null at 240 preserves the generated function-entry fast path too.
    void (*extended_view_init)(std::uint32_t extra_left,
                               std::uint32_t extra_right) = nullptr;

    // ---- pre-boot launcher identity (launcher_seam.h, RECOMP_LAUNCHER builds) --
    // Consumed by the recomp-ui launcher seam a game's main() runs BEFORE
    // run_game(); the runtime itself never reads these. All optional.
    const char* launcher_region = nullptr;      // display region, e.g. "USA"
    const char* launcher_save_path = nullptr;   // explicit save file (game.toml
                                                // [save].path); null => <rom>.sav
                                                // derived from the seeded ROM
    // >240 offers the launcher's 16:9 widescreen toggle, mapped to
    // --view-width <this> when enabled (e.g. Mega Man Zero's 480 extended
    // view). 0/240 = no widescreen surface shown.
    std::uint16_t widescreen_view_width = 0;
};

int run_game(int argc, char** argv, const RunOptions& opts = {});

}  // namespace gbarecomp
