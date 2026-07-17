// launcher_seam.h — the recomp-ui pre-boot launcher seam for GBA games.
//
// Header-only and ONLY meaningful under RECOMP_LAUNCHER: a game target that
// calls recomp_target_launcher_ui() (recomp-ui/recomp_ui.cmake) gets that
// define plus the recomp-ui include dirs; its main() then runs
// gbarecomp_launcher_preboot() BEFORE gbarecomp::run_game():
//
//     gbarecomp::RunOptions opts;  ...fill identity...
//   #if defined(RECOMP_LAUNCHER)
//     std::vector<std::string> args(argv, argv + argc);
//     if (gbarecomp_launcher_preboot(args, opts)) return 0;   // user quit
//     std::vector<char*> av;
//     for (auto& s : args) av.push_back(s.data());
//     return gbarecomp::run_game((int)av.size(), av.data(), opts);
//   #else
//     return gbarecomp::run_game(argc, argv, opts);
//   #endif
//
// The seam translates launcher output into ordinary CLI args (--rom, --bios,
// --scale, --screen, --volume, --linear-filter, --fullscreen, --view-width),
// so run_game() needs no launcher knowledge: the same runtime path handles
// launcher-driven and hand-typed invocations identically. Settings persist in
// <exe>/config.ini [Launcher] (player-owned; the identity-gated game.toml is
// never written). Player keybinds + [KeyMap] hotkeys are persisted by the
// launcher itself (keybinds.ini / config.ini next to the exe — the same files
// host_window.cpp reads).
//
// The launcher is skipped (returns 0 with args untouched) for every headless
// or explicit-path invocation: --tcp, --steps, --frames, --no-window,
// --dump-bmp/--dump-png, --load-state, --help, an explicit --rom, or
// GBARECOMP_NO_LAUNCHER=1. `--launcher` forces it past a persisted
// skip_launcher=1; `--no-launcher` skips for one run. Both are seam-only
// flags, stripped before run_game() parses the args.

#pragma once

#if defined(RECOMP_LAUNCHER)

#include "runtime.h"

#include "recomp_launcher.h"    // recomp-ui C ABI (include dir via recomp_ui.cmake)
#include "launcher_profile.h"   // launcher_profile_apply("gba", ...)

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace gbarecomp_seam {

// gbarecomp's screen-model tokens, in the launcher's kGbaScreenKindNames
// index order (Raw/Unlit/Frontlit/Backlit/Classic).
inline const char* const kScreenTokens[5] = {
    "raw", "unlit", "frontlit", "backlit", "classic"
};

inline int screen_token_to_index(const std::string& tok) {
    for (int i = 0; i < 5; ++i)
        if (tok == kScreenTokens[i]) return i;
    return 0;
}

inline std::string exe_dir(const std::vector<std::string>& args) {
    if (!args.empty() && !args[0].empty()) {
        std::filesystem::path p(args[0]);
        if (p.has_parent_path()) return p.parent_path().string();
    }
    return ".";
}

inline std::string read_single_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (f && std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                 line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        return line;
    }
    return {};
}

inline void write_single_line(const std::string& path, const std::string& value) {
    if (value.empty()) return;
    std::ofstream f(path, std::ios::trunc);
    if (f) f << value << "\n";
}

// Minimal reader for a single `key = "value"` under [section] in game.toml —
// enough to prefill the launcher from [rom].path / [bios].path without
// pulling in the full TOML parser. Returns the value resolved relative to the
// toml's own directory (matching the runtime's resolve_config_path), or "".
inline std::string toml_path_value(const std::string& toml_path,
                                   const char* section, const char* key) {
    std::ifstream f(toml_path);
    if (!f) return {};
    std::string line, cur;
    std::string found;
    while (std::getline(f, line)) {
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        if (line[b] == '#' || line[b] == ';') continue;
        if (line[b] == '[') {
            size_t e = line.find(']', b);
            if (e != std::string::npos) cur = line.substr(b + 1, e - b - 1);
            continue;
        }
        if (cur != section) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(b, eq - b);
        size_t ke = k.find_last_not_of(" \t");
        if (ke != std::string::npos) k = k.substr(0, ke + 1);
        if (k != key) continue;
        std::string v = line.substr(eq + 1);
        size_t vb = v.find('"');
        size_t ve = v.rfind('"');
        if (vb == std::string::npos || ve == vb) continue;
        found = v.substr(vb + 1, ve - vb - 1);
        break;
    }
    if (found.empty()) return {};
    // Resolve relative to the toml's directory.
    std::filesystem::path p(found);
    if (p.is_absolute()) return p.string();
    std::filesystem::path base = std::filesystem::path(toml_path).parent_path();
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::weakly_canonical(base / p, ec);
    return (ec ? (base / p) : abs).string();
}

// Launcher-owned persisted settings (config.ini [Launcher]).
struct SeamConfig {
    int  scale = 3;
    int  fullscreen = 0;
    int  linear_filter = 0;
    int  screen_kind = 0;      // kScreenTokens index
    int  volume = 100;
    int  skip_launcher = 0;
    int  widescreen = 0;       // games with widescreen_view_width only
    int  aspect_index = 0;     // games with a launcher_aspect vocabulary only
};

inline void seam_config_load(const std::string& path, SeamConfig* c) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    bool in_section = false;
    while (std::getline(f, line)) {
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        std::string s = line.substr(b, e - b + 1);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        if (s[0] == '[') {
            in_section = s.rfind("[Launcher]", 0) == 0;
            continue;
        }
        if (!in_section) continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string v) {
            size_t vb = v.find_first_not_of(" \t");
            if (vb == std::string::npos) return std::string();
            size_t ve = v.find_last_not_of(" \t");
            return v.substr(vb, ve - vb + 1);
        };
        std::string key = trim(s.substr(0, eq));
        std::string val = trim(s.substr(eq + 1));
        if (key == "scale")         c->scale = std::atoi(val.c_str());
        else if (key == "fullscreen")    c->fullscreen = std::atoi(val.c_str());
        else if (key == "linear_filter") c->linear_filter = std::atoi(val.c_str());
        else if (key == "screen")        c->screen_kind = screen_token_to_index(val);
        else if (key == "volume")        c->volume = std::atoi(val.c_str());
        else if (key == "skip_launcher") c->skip_launcher = std::atoi(val.c_str());
        else if (key == "widescreen")    c->widescreen = std::atoi(val.c_str());
        else if (key == "aspect_index")  c->aspect_index = std::atoi(val.c_str());
    }
    if (c->scale < 1) c->scale = 1;
    if (c->scale > 8) c->scale = 8;
    if (c->volume < 0) c->volume = 0;
    if (c->volume > 100) c->volume = 100;
    if (c->screen_kind < 0 || c->screen_kind > 4) c->screen_kind = 0;
}

// Rewrite ONLY the [Launcher] section of config.ini, preserving every other
// line (notably [KeyMap], which the launcher's hotkey editor edits in place).
inline void seam_config_save(const std::string& path, const SeamConfig& c) {
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    // Strip the existing [Launcher] section body.
    std::vector<std::string> kept;
    bool in_section = false;
    for (const auto& l : lines) {
        size_t b = l.find_first_not_of(" \t");
        bool is_header = b != std::string::npos && l[b] == '[';
        if (is_header) in_section = l.compare(b, 10, "[Launcher]") == 0;
        if (is_header && in_section) continue;
        if (in_section && !is_header) continue;
        kept.push_back(l);
    }
    while (!kept.empty() && kept.back().empty()) kept.pop_back();

    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    for (const auto& l : kept) f << l << "\n";
    if (!kept.empty()) f << "\n";
    f << "[Launcher]\n";
    f << "scale = " << c.scale << "\n";
    f << "fullscreen = " << c.fullscreen << "\n";
    f << "linear_filter = " << c.linear_filter << "\n";
    f << "screen = " << kScreenTokens[c.screen_kind] << "\n";
    f << "volume = " << c.volume << "\n";
    f << "skip_launcher = " << c.skip_launcher << "\n";
    f << "widescreen = " << c.widescreen << "\n";
    f << "aspect_index = " << c.aspect_index << "\n";
}

// Append the persisted/committed settings as ordinary run_game() CLI args.
inline void seam_append_setting_args(std::vector<std::string>& args,
                                     const SeamConfig& c,
                                     const gbarecomp::RunOptions& opts) {
    args.push_back("--scale");
    args.push_back(std::to_string(c.scale));
    args.push_back("--volume");
    args.push_back(std::to_string(c.volume));
    args.push_back("--linear-filter");
    args.push_back(c.linear_filter ? "1" : "0");
    args.push_back("--screen");
    args.push_back(kScreenTokens[c.screen_kind]);
    if (c.fullscreen) args.push_back("--fullscreen");
    if (opts.launcher_num_aspects > 0 && opts.launcher_aspect_view_widths) {
        // Aspect vocabulary (multi-width games): committed index -> width.
        int idx = c.aspect_index;
        if (idx < 0 || idx >= opts.launcher_num_aspects) idx = 0;
        const std::uint16_t w = opts.launcher_aspect_view_widths[idx];
        if (w > 240) {
            args.push_back("--view-width");
            args.push_back(std::to_string(w));
        }
    } else if (c.widescreen && opts.widescreen_view_width > 240) {
        args.push_back("--view-width");
        args.push_back(std::to_string(opts.widescreen_view_width));
    }
}

}  // namespace gbarecomp_seam

// Run the pre-boot launcher. Returns 1 if the user quit (caller returns 0
// from main without booting), 0 to continue into run_game() — with `args`
// extended by the launcher-committed settings when the launcher ran.
inline int gbarecomp_launcher_preboot(std::vector<std::string>& args,
                                      const gbarecomp::RunOptions& opts) {
    using namespace gbarecomp_seam;

    // ---- skip decisions -----------------------------------------------------
    bool force_launcher = false;
    bool skip_once = false;
    bool headless = false;
    {
        std::vector<std::string> filtered;
        filtered.reserve(args.size());
        for (size_t i = 0; i < args.size(); ++i) {
            const std::string& s = args[i];
            if (i > 0 && s == "--launcher")    { force_launcher = true; continue; }
            if (i > 0 && s == "--no-launcher") { skip_once = true; continue; }
            if (i > 0 && (s == "--tcp" || s == "--steps" || s == "--frames" ||
                          s == "--no-window" || s == "--dump-bmp" ||
                          s == "--dump-png" || s == "--load-state" ||
                          s == "--rom" || s == "--help" || s == "-h"))
                headless = true;
            filtered.push_back(s);
        }
        args.swap(filtered);
    }
    if (const char* env = std::getenv("GBARECOMP_NO_LAUNCHER"))
        if (env[0] && env[0] != '0') skip_once = true;
    if (headless || (skip_once && !force_launcher)) return 0;

    const std::string dir = exe_dir(args);
    const std::string config_path = dir + "/config.ini";
    const std::string keybinds_path = dir + "/keybinds.ini";

    SeamConfig cfg;
    seam_config_load(config_path, &cfg);
    if (cfg.skip_launcher && !force_launcher) {
        // Boot straight in, but still honor the persisted settings.
        seam_append_setting_args(args, cfg, opts);
        return 0;
    }

    // ---- seed the launcher: last pick (rom.cfg/bios.cfg) -> game.toml -------
    // so a first run (no sidecar yet) still prefills from the paths the game
    // already declares, instead of opening blank and forcing a re-browse.
    const std::string rom_cfg  = dir + "/rom.cfg";
    const std::string bios_cfg = dir + "/bios.cfg";
    std::string seed_rom  = read_single_line(rom_cfg);
    std::string seed_bios = read_single_line(bios_cfg);
    if ((seed_rom.empty() || seed_bios.empty()) &&
        opts.launcher_game_config && opts.launcher_game_config[0]) {
        // game.toml path is relative to the exe/CWD; try both.
        std::string toml = opts.launcher_game_config;
        if (!std::filesystem::exists(toml)) {
            std::string alt = dir + "/" + toml;
            if (std::filesystem::exists(alt)) toml = alt;
        }
        if (std::filesystem::exists(toml)) {
            if (seed_rom.empty())  seed_rom  = toml_path_value(toml, "rom", "path");
            if (seed_bios.empty()) seed_bios = toml_path_value(toml, "bios", "path");
        }
    }

    RecompLauncherCSettings ls;
    std::memset(&ls, 0, sizeof(ls));
    ls.output_method = 2;                 // launcher UI renders via OpenGL
    ls.window_scale  = cfg.scale;
    ls.fullscreen    = cfg.fullscreen;
    ls.linear_filter = cfg.linear_filter;
    ls.widescreen    = cfg.widescreen;
    ls.enable_audio  = 1;
    ls.audio_freq    = 32768;             // GBA mixer base rate (display only)
    ls.volume        = cfg.volume;
    ls.player_src[0] = 1;                 // keyboard (gbarecomp host input)
    ls.skip_launcher = cfg.skip_launcher;
    ls.screen_kind   = cfg.screen_kind;
    ls.aspect_index  = cfg.aspect_index;
    std::snprintf(ls.bios_path, sizeof(ls.bios_path), "%s", seed_bios.c_str());

    RecompLauncherCGameInfo gi;
    std::memset(&gi, 0, sizeof(gi));
    launcher_profile_apply("gba", &gi);   // GBA identity + capability defaults
    gi.name = opts.builtin_game_name ? opts.builtin_game_name : "GBA cartridge";
    gi.region = opts.launcher_region;
    // SHA-1 is gbarecomp's real ROM identity gate — hand the launcher the
    // SAME fingerprint so its "verified" check agrees with the runtime (a
    // CRC32 is dump-specific and would reject other valid dumps). CRC32, when
    // present, stays informational only.
    const char* sha1_one[1];
    if (opts.builtin_rom_sha1 && opts.builtin_rom_sha1[0]) {
        sha1_one[0] = opts.builtin_rom_sha1;
        gi.known_sha1_hex = sha1_one;
        gi.num_known_sha1 = 1;
    }
    gi.widescreen_supported = opts.widescreen_view_width > 240 ? 1 : 0;
    gi.config_path = config_path.c_str();
    gi.keybinds_path = keybinds_path.c_str();
    gi.boxart_path = opts.launcher_boxart;   // NULL => default assets/img/boxart.tga
    if (opts.launcher_num_aspects > 0 && opts.launcher_aspect_labels) {
        // Multi-width extended view: game-supplied aspect cycle, tagged
        // EXPERIMENTAL (the snesrecomp/psxrecomp widescreen convention).
        gi.aspect_labels       = opts.launcher_aspect_labels;
        gi.num_aspect_labels   = opts.launcher_num_aspects;
        gi.aspect_experimental = 1;
        gi.widescreen_supported = 0;   // the cycle supersedes the bool toggle
    }
    // Save row: the explicit per-game save path when the game declares one,
    // else the runtime's <rom>.sav convention derived from the seeded ROM.
    std::string save_display;
    if (opts.launcher_save_path && opts.launcher_save_path[0]) {
        save_display = opts.launcher_save_path;
    } else if (!seed_rom.empty()) {
        std::filesystem::path p(seed_rom);
        p.replace_extension(".sav");
        save_display = p.string();
    }
    if (!save_display.empty()) gi.sram_path = save_display.c_str();

    std::string title = std::string(gi.name) + " \xE2\x80\x94 Launcher";

    char picked_rom[1024] = {0};
    int rc = recomp_launcher_run_window(title.c_str(), &ls, &gi, dir.c_str(),
                                        seed_rom.c_str(),
                                        picked_rom, sizeof(picked_rom));
    if (rc == 1) return 1;    // user closed the launcher: quit without booting
    if (rc != 0) return 0;    // unavailable: fall back to the asset picker

    // ---- persist + translate the committed settings -------------------------
    cfg.scale         = ls.window_scale > 0 ? ls.window_scale : cfg.scale;
    cfg.fullscreen    = ls.fullscreen ? 1 : 0;
    cfg.linear_filter = ls.linear_filter ? 1 : 0;
    cfg.screen_kind   = (ls.screen_kind >= 0 && ls.screen_kind <= 4)
                          ? ls.screen_kind : 0;
    cfg.volume        = ls.volume;
    cfg.skip_launcher = ls.skip_launcher ? 1 : 0;
    cfg.widescreen    = ls.widescreen ? 1 : 0;
    cfg.aspect_index  = (opts.launcher_num_aspects > 0 &&
                         ls.aspect_index >= 0 &&
                         ls.aspect_index < opts.launcher_num_aspects)
                          ? ls.aspect_index : 0;
    seam_config_save(config_path, cfg);

    if (picked_rom[0]) {
        args.push_back("--rom");
        args.push_back(picked_rom);
        // Persist the pick NOW (not only after a successful boot) so the next
        // launch prefills instead of re-prompting — the whole point of the
        // launcher remembering. Mirrors the runtime asset picker's cache.
        write_single_line(rom_cfg, picked_rom);
    }
    if (ls.bios_path[0]) {
        args.push_back("--bios");
        args.push_back(ls.bios_path);
        write_single_line(bios_cfg, ls.bios_path);
    }
    seam_append_setting_args(args, cfg, opts);
    return 0;
}

#endif  // RECOMP_LAUNCHER
