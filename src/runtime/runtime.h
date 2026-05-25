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
};

int run_game(int argc, char** argv, const RunOptions& opts = {});

}  // namespace gbarecomp
