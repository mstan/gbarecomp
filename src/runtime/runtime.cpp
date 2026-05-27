// runtime.cpp - shared BIOS+ROM runner for generated game binaries.
//
// This translation unit is intentionally interpreter-free. Per
// PRINCIPLES.md "Interpreter is informative, never load-bearing
// (SHOWSTOPPER)", the exec path here is recompiler-only: it
// initializes the recomp ABI state (g_cpu via runtime_arm.h) and
// calls runtime_dispatch(pc). If the recompiler hasn't lowered an
// IrOp or hasn't discovered a function, the appropriate runtime_*
// helper aborts loudly — that abort is the gate. Never paper over
// it by routing to armv4t::Interpreter.
//
// Scaffolding kept across the carve from Codex's spike: TOML config
// loader, CLI parser, BIOS+ROM SHA verification, ROM header parse,
// bus + PPU + EEPROM setup, HostWindow, BMP dump, TCP server hook.
// The exec loop itself is a placeholder until Phase C (per-IrOp
// codegen) + Phase B (dispatch wire-up) land — at which point
// step_once() becomes a real `runtime_dispatch` driver.

#include "runtime.h"

#include "asset_picker.h"
#include "gba_bios.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "gba_rom_header.h"
#include "host_window.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "sha1.h"
#include "snapshot.h"
#include "tcp_debug_server.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#ifndef GBARECOMP_DEFAULT_GAME_CONFIG
#define GBARECOMP_DEFAULT_GAME_CONFIG "game.toml"
#endif

#ifndef GBARECOMP_WINDOW_TITLE
#define GBARECOMP_WINDOW_TITLE "gbarecomp"
#endif

#ifndef GBARECOMP_DEFAULT_DEBUG_PORT
#define GBARECOMP_DEFAULT_DEBUG_PORT 0
#endif

namespace gbarecomp {
namespace {

constexpr std::size_t kMaxRomSize = 32u * 1024u * 1024u;

struct Args {
    std::string config = GBARECOMP_DEFAULT_GAME_CONFIG;
    std::string game_short_name;
    std::string bios;
    std::string bios_sha1 = gba::GbaBios::kExpectedSha1;
    // SHA-1 is the gate (40 hex chars, way stronger than a 32-bit
    // CRC). CRC32 is left at 0 = "no check" — the asset picker will
    // still compute + display it, but only SHA-1 mismatch raises the
    // warning dialog.
    std::uint32_t bios_crc32 = 0;
    std::string rom;
    std::string rom_sha1;
    std::uint32_t rom_crc32 = 0;  // 0 = no CRC check (per-game TOML fills)
    std::string save_path;
    std::size_t save_size = 0;
    int steps = 16;
    int frames = -1;
    int scale = 3;
    int tcp_port = 0;
    bool steps_set = false;
    bool frames_set = false;
    bool window_set = false;
    bool quiet = false;
    bool window = false;
    std::string dump_bmp;
};

std::string trim(std::string_view in) {
    std::size_t first = 0;
    while (first < in.size() &&
           std::isspace(static_cast<unsigned char>(in[first]))) {
        ++first;
    }
    std::size_t last = in.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(in[last - 1]))) {
        --last;
    }
    return std::string(in.substr(first, last - first));
}

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string strip_comment(std::string_view line) {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') quoted = !quoted;
        if (line[i] == '#' && !quoted) return std::string(line.substr(0, i));
    }
    return std::string(line);
}

std::string unquote(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        return v.substr(1, v.size() - 2);
    }
    return v;
}

bool parse_int(std::string_view text, int* out) {
    std::string s(text);
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 0);
    if (end == s.c_str() || *end != '\0') return false;
    if (v < std::numeric_limits<int>::min() ||
        v > std::numeric_limits<int>::max()) {
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}

bool parse_u64(std::string_view text, uint64_t* out) {
    std::string s(text);
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    if (end == s.c_str() || *end != '\0') return false;
    *out = static_cast<uint64_t>(v);
    return true;
}

// Parse a hex CRC32 from TOML/CLI. Accepts "0x21A2AE0A", "21A2AE0A",
// "21a2ae0a". Returns 0 on unparseable input (which means "no CRC
// check" downstream — that's the intended fallback).
std::uint32_t parse_hex_u32(const std::string& s) {
    if (s.empty()) return 0;
    const char* p = s.c_str();
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    char* end = nullptr;
    unsigned long long v = std::strtoull(p, &end, 16);
    if (end == p || *end != '\0') return 0;
    if (v > 0xFFFFFFFFull) return 0;
    return static_cast<std::uint32_t>(v);
}

std::string resolve_config_path(const std::filesystem::path& base,
                                const std::string& value) {
    std::filesystem::path p(value);
    if (p.is_relative()) p = base / p;
    return p.lexically_normal().string();
}

bool read_file(const std::string& path, std::vector<uint8_t>* out,
               std::string* err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        if (err) *err = "could not open " + path;
        return false;
    }
    std::streamoff size = f.tellg();
    if (size < 0) {
        if (err) *err = "could not stat " + path;
        return false;
    }
    out->assign(static_cast<std::size_t>(size), 0);
    f.seekg(0, std::ios::beg);
    if (!out->empty()) {
        f.read(reinterpret_cast<char*>(out->data()),
               static_cast<std::streamsize>(out->size()));
    }
    if (!f) {
        if (err) *err = "could not read " + path;
        return false;
    }
    return true;
}

bool write_bmp(const std::string& path, const uint8_t* rgb,
               uint32_t w, uint32_t h) {
    uint32_t row_bytes = w * 3;
    uint32_t padded_row = (row_bytes + 3) & ~3u;
    uint32_t pixel_data_size = padded_row * h;
    uint32_t file_size = 14 + 40 + pixel_data_size;

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    auto put_u16 = [&](uint16_t v) {
        uint8_t b[2] = {
            static_cast<uint8_t>(v & 0xFF),
            static_cast<uint8_t>((v >> 8) & 0xFF),
        };
        f.write(reinterpret_cast<const char*>(b), 2);
    };
    auto put_u32 = [&](uint32_t v) {
        uint8_t b[4] = {
            static_cast<uint8_t>(v & 0xFF),
            static_cast<uint8_t>((v >> 8) & 0xFF),
            static_cast<uint8_t>((v >> 16) & 0xFF),
            static_cast<uint8_t>((v >> 24) & 0xFF),
        };
        f.write(reinterpret_cast<const char*>(b), 4);
    };

    f.write("BM", 2);
    put_u32(file_size);
    put_u16(0);
    put_u16(0);
    put_u32(14 + 40);
    put_u32(40);
    put_u32(w);
    put_u32(h);
    put_u16(1);
    put_u16(24);
    put_u32(0);
    put_u32(pixel_data_size);
    put_u32(2835);
    put_u32(2835);
    put_u32(0);
    put_u32(0);

    std::vector<uint8_t> row(padded_row, 0);
    for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* src = rgb + (static_cast<uint32_t>(y) * w + x) * 3;
            row[x * 3 + 0] = src[2];
            row[x * 3 + 1] = src[1];
            row[x * 3 + 2] = src[0];
        }
        f.write(reinterpret_cast<const char*>(row.data()), padded_row);
    }
    return static_cast<bool>(f);
}

bool write_file(const std::string& path, const std::vector<uint8_t>& bytes,
                std::string* err) {
    std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            if (err) *err = "could not create save directory " +
                            p.parent_path().string() + ": " + ec.message();
            return false;
        }
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (err) *err = "could not open save file for write " + path;
        return false;
    }
    if (!bytes.empty()) {
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    if (!f) {
        if (err) *err = "could not write save file " + path;
        return false;
    }
    return true;
}

bool apply_toml_file(const std::filesystem::path& path, Args* args,
                     std::string* default_region, std::string* err) {
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "could not open config " + path.string();
        return false;
    }

    const std::filesystem::path base = path.parent_path();
    std::string section;
    std::string raw;
    while (std::getline(f, raw)) {
        std::string line = trim(strip_comment(raw));
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(std::string_view(line).substr(1, line.size() - 2));
            continue;
        }

        std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(std::string_view(line).substr(0, eq));
        std::string val = unquote(line.substr(eq + 1));
        if (val == "TBD") val.clear();

        if (section == "game" && key == "short_name") {
            args->game_short_name = val;
        } else if (section == "game" && key == "default_region") {
            if (default_region) *default_region = val;
        } else if (section == "bios" && key == "path" && !val.empty()) {
            args->bios = resolve_config_path(base, val);
        } else if (section == "bios" && key == "sha1") {
            args->bios_sha1 = lower_ascii(val);
        } else if (section == "bios" && key == "crc32") {
            args->bios_crc32 = parse_hex_u32(val);
        } else if (section == "rom" && key == "path" && !val.empty()) {
            args->rom = resolve_config_path(base, val);
        } else if (section == "rom" && key == "sha1") {
            args->rom_sha1 = lower_ascii(val);
        } else if (section == "rom" && key == "crc32") {
            args->rom_crc32 = parse_hex_u32(val);
        } else if (section == "save" && key == "path" && !val.empty()) {
            args->save_path = resolve_config_path(base, val);
        } else if (section == "save" && key == "size" && !val.empty()) {
            uint64_t n = 0;
            if (!parse_u64(val, &n) ||
                n > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
                if (err) *err = "invalid [save].size value in " + path.string();
                return false;
            }
            args->save_size = static_cast<std::size_t>(n);
        }
    }
    return true;
}

void find_config_arg(int argc, char** argv, Args* args) {
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--config" && i + 1 < argc) {
            args->config = argv[++i];
            continue;
        }
        if ((s == "--bios" || s == "--rom" || s == "--bios-sha1" ||
             s == "--rom-sha1" || s == "--steps" || s == "--frames" ||
             s == "--scale" || s == "--tcp" || s == "--dump-bmp" ||
             s == "--save" || s == "--save-path") &&
            i + 1 < argc) {
            ++i;
            continue;
        }
        if (!s.empty() && s[0] != '-') {
            args->config = s;
        }
    }
}

bool load_config(Args* args, std::string* err) {
    if (args->config.empty()) return true;
    std::filesystem::path config_path(args->config);
    if (!std::filesystem::exists(config_path)) {
        if (args->config == std::string(GBARECOMP_DEFAULT_GAME_CONFIG)) {
            return true;
        }
        if (err) *err = "config file does not exist: " + args->config;
        return false;
    }

    config_path = std::filesystem::absolute(config_path).lexically_normal();
    std::string default_region;
    if (!apply_toml_file(config_path, args, &default_region, err)) return false;

    if (!default_region.empty()) {
        std::vector<std::filesystem::path> overlays;
        overlays.push_back(config_path.parent_path() / "config" /
                           (default_region + ".toml"));
        if (!args->game_short_name.empty()) {
            overlays.push_back(config_path.parent_path() / "config" /
                               (args->game_short_name + "_" +
                                default_region + ".toml"));
            std::string compact = args->game_short_name;
            compact.erase(std::remove(compact.begin(), compact.end(), '_'),
                          compact.end());
            if (compact != args->game_short_name) {
                overlays.push_back(config_path.parent_path() / "config" /
                                   (compact + "_" + default_region + ".toml"));
            }
        }
        for (const auto& overlay : overlays) {
            if (std::filesystem::exists(overlay)) {
                if (!apply_toml_file(overlay, args, nullptr, err)) return false;
            }
        }
    }
    return true;
}

bool parse_cli(int argc, char** argv, Args* args, std::string* err) {
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                if (err) *err = std::string("missing value for ") + name;
                return nullptr;
            }
            return argv[++i];
        };

        if (s == "--config") {
            if (!need_value("--config")) return false;
            continue;
        }
        if (s == "--bios") {
            const char* v = need_value("--bios");
            if (!v) return false;
            args->bios = v;
            continue;
        }
        if (s == "--rom") {
            const char* v = need_value("--rom");
            if (!v) return false;
            args->rom = v;
            continue;
        }
        if (s == "--bios-sha1") {
            const char* v = need_value("--bios-sha1");
            if (!v) return false;
            args->bios_sha1 = lower_ascii(v);
            continue;
        }
        if (s == "--rom-sha1") {
            const char* v = need_value("--rom-sha1");
            if (!v) return false;
            args->rom_sha1 = lower_ascii(v);
            continue;
        }
        if (s == "--steps") {
            const char* v = need_value("--steps");
            if (!v) return false;
            if (!parse_int(v, &args->steps)) {
                if (err) *err = "invalid --steps value";
                return false;
            }
            args->steps_set = true;
            continue;
        }
        if (s == "--frames") {
            const char* v = need_value("--frames");
            if (!v) return false;
            if (!parse_int(v, &args->frames)) {
                if (err) *err = "invalid --frames value";
                return false;
            }
            args->frames_set = true;
            continue;
        }
        if (s == "--scale") {
            const char* v = need_value("--scale");
            if (!v) return false;
            if (!parse_int(v, &args->scale)) {
                if (err) *err = "invalid --scale value";
                return false;
            }
            continue;
        }
        if (s == "--tcp") {
            const char* v = need_value("--tcp");
            if (!v) return false;
            if (!parse_int(v, &args->tcp_port)) {
                if (err) *err = "invalid --tcp value";
                return false;
            }
            args->quiet = true;
            continue;
        }
        if (s == "--dump-bmp") {
            const char* v = need_value("--dump-bmp");
            if (!v) return false;
            args->dump_bmp = v;
            continue;
        }
        if (s == "--save" || s == "--save-path") {
            const char* v = need_value(s.c_str());
            if (!v) return false;
            args->save_path = v;
            continue;
        }
        if (s == "--quiet") {
            args->quiet = true;
            continue;
        }
        if (s == "--window") {
            args->window = true;
            args->window_set = true;
            args->quiet = true;
            continue;
        }
        if (s == "--no-window") {
            args->window = false;
            args->window_set = true;
            continue;
        }
        if (s == "--help" || s == "-h") {
            continue;
        }
        if (!s.empty() && s[0] != '-') {
            continue;
        }
        if (err) *err = "unknown argument " + s;
        return false;
    }
    return true;
}

// Initialize the C-ABI CPU state (visible to recompiled code) to
// GBA reset: SVC mode, I/F masked, ARM state, PC=0, SP set to
// the post-BIOS stack base (recompiled BIOS will reprogram banked
// SPs to their canonical values).
void reset_recomp_cpu() {
    runtime_trace_reset();
    for (int i = 0; i < 16; ++i) g_cpu.R[i] = 0;
    g_cpu.R[13] = 0x03007FE0;
    g_cpu.cpsr = CPSR_I_BIT | CPSR_F_BIT | 0x13u /* SVC */;
}

std::size_t count_nonzero(const uint8_t* p, std::size_t n) {
    std::size_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != 0) ++c;
    }
    return c;
}

}  // namespace

int run_game(int argc, char** argv, const RunOptions& opts) {
    Args args;

    // Seed built-in defaults from the caller (the per-game runner).
    // CLI / TOML can still override these.
    if (opts.builtin_game_name && *opts.builtin_game_name) {
        args.game_short_name = opts.builtin_game_name;
    }
    if (opts.builtin_rom_sha1 && *opts.builtin_rom_sha1) {
        args.rom_sha1 = lower_ascii(opts.builtin_rom_sha1);
    }
    if (opts.builtin_rom_crc32 != 0) {
        args.rom_crc32 = opts.builtin_rom_crc32;
    }

    find_config_arg(argc, argv, &args);

    std::string err;
    // load_config is tolerant of a missing TOML — standalone .exe
    // releases ship without one and rely on the built-in defaults
    // above plus the picker. A malformed TOML still fails hard.
    if (!load_config(&args, &err)) {
        std::error_code ec;
        if (std::filesystem::exists(args.config, ec)) {
            std::fprintf(stderr, "[gbarecomp:runtime] %s\n", err.c_str());
            return 1;
        }
    }
    if (!parse_cli(argc, argv, &args, &err)) {
        std::fprintf(stderr, "[gbarecomp:runtime] %s\n", err.c_str());
        return 1;
    }

    // Resolve BIOS via the picker chain (argv path -> sidecar cache ->
    // Win32 file dialog). Released binaries don't ship a default path
    // that exists, so the picker is what makes a fresh install work
    // without a CLI argument. CRC32 + SHA-1 mismatch is a soft warning
    // so the user can boot with an uncatalogued region/revision; wrong
    // size is a hard fail (see asset_picker.cpp).
    {
        AssetSpec spec;
        spec.display_name   = "GBA BIOS";
        spec.dialog_filter  = "GBA BIOS (*.bin;*.BIN)\0*.bin;*.BIN\0"
                              "All Files (*.*)\0*.*\0";
        spec.dialog_title   = "Select your GBA BIOS dump (gba_bios.bin)";
        spec.cache_filename = "bios.cfg";
        spec.expected_size  = gba::GbaBios::kSize;
        spec.expected_sha1  = args.bios_sha1.empty()
                                ? gba::GbaBios::kExpectedSha1
                                : args.bios_sha1.c_str();
        spec.expected_crc32 = args.bios_crc32;
        auto r = resolve_asset(args.bios, spec, argv[0]);
        if (!r.ok) {
            std::fprintf(stderr,
                         "[gbarecomp:runtime] BIOS resolution failed: %s\n",
                         r.error.c_str());
            return 1;
        }
        args.bios = r.path;
    }

    // Resolve ROM the same way. The per-game TOML supplies the
    // expected hash + (optionally) CRC32 — both come pre-filled via
    // load_config.
    if (args.rom_sha1.empty()) {
        std::fprintf(stderr,
                     "[gbarecomp:runtime] missing expected ROM SHA-1; "
                     "refusing to launch (per-game TOML must set "
                     "[rom].sha1)\n");
        return 1;
    }
    {
        AssetSpec spec;
        spec.display_name   = args.game_short_name.empty()
                                ? "game ROM" : args.game_short_name.c_str();
        spec.dialog_filter  = "GBA ROM (*.gba;*.GBA)\0*.gba;*.GBA\0"
                              "All Files (*.*)\0*.*\0";
        spec.dialog_title   = "Select the game ROM (.gba)";
        spec.cache_filename = "rom.cfg";
        spec.expected_size  = 0;  // GBA ROMs vary in size; SHA-1 covers it
        spec.expected_sha1  = args.rom_sha1.c_str();
        spec.expected_crc32 = args.rom_crc32;
        auto r = resolve_asset(args.rom, spec, argv[0]);
        if (!r.ok) {
            std::fprintf(stderr,
                         "[gbarecomp:runtime] ROM resolution failed: %s\n",
                         r.error.c_str());
            return 1;
        }
        args.rom = r.path;
    }

    if (!args.steps_set && !args.frames_set && args.tcp_port <= 0) {
        // Auto-window when nothing was specified and SDL is built in.
        if (!args.window_set && HostWindow::is_available()) {
            args.window = true;
            args.quiet = true;
        }
        // Headless fallback: one frame is enough to validate the runtime
        // came up. Explicit --window runs stay open-ended (let the user
        // close the window).
        if (!args.window) {
            args.frames = 1;
            args.frames_set = true;
        }
    }
    if (!args.steps_set && (args.window || args.frames >= 0 || args.tcp_port > 0)) {
        args.steps = std::numeric_limits<int>::max() / 2;
    }

    // The picker has already validated SHA-1 (warn-and-try). Pass an
    // empty expected hash here so a non-canonical-but-warned dump
    // doesn't trip a hard fail in the loader.
    gba::GbaBios bios;
    if (!bios.load_from_file(args.bios, std::string{}, &err)) {
        std::fprintf(stderr, "[gbarecomp:runtime] %s\n", err.c_str());
        return 1;
    }

    std::vector<uint8_t> rom;
    if (!read_file(args.rom, &rom, &err)) {
        std::fprintf(stderr, "[gbarecomp:runtime] %s\n", err.c_str());
        return 1;
    }
    if (rom.size() < 0xC0 || rom.size() > kMaxRomSize) {
        std::fprintf(stderr,
                     "[gbarecomp:runtime] invalid ROM size: %zu bytes\n",
                     rom.size());
        return 1;
    }
    std::string rom_sha1 = lower_ascii(gba::sha1(rom.data(), rom.size()).hex());
    if (rom_sha1 != lower_ascii(args.rom_sha1)) {
        std::fprintf(stderr,
                     "[gbarecomp:runtime] ROM SHA-1 mismatch: got %s expected %s\n",
                     rom_sha1.c_str(), args.rom_sha1.c_str());
        return 1;
    }

    gba::RomHeader header = gba::parse_rom(rom.data(), rom.size());
    if (!header.ok) {
        std::fprintf(stderr, "[gbarecomp:runtime] invalid ROM header: %s\n",
                     header.error.c_str());
        return 1;
    }

    if (!args.quiet) {
        std::printf("bios_loaded sha1=%s size=%zu\n",
                    bios.sha1_hex().c_str(), gba::GbaBios::kSize);
        std::printf("rom_loaded sha1=%s size=%zu title=\"%s\" code=%s "
                    "entry=0x%08x save=%s signature=%s\n",
                    rom_sha1.c_str(), rom.size(), header.game_title.c_str(),
                    header.game_code.c_str(), header.entry_target,
                    gba::save_type_name(header.save_type),
                    header.save_signature.c_str());
    }

    gba::GbaBus bus;
    gba::GbaPpu ppu;
    bus.set_bios(&bios);
    bus.set_rom(rom.data(), rom.size());
    if (header.save_type == gba::SaveType::EEPROM) {
        std::size_t eeprom_bytes = args.save_size ? args.save_size : (8 * 1024);
        bus.save().configure_eeprom(eeprom_bytes);

        if (args.save_path.empty()) {
            std::filesystem::path save_path(args.rom);
            save_path.replace_extension(".sav");
            args.save_path = save_path.string();
        }

        std::error_code ec;
        if (!args.save_path.empty() &&
            std::filesystem::exists(args.save_path, ec)) {
            std::vector<uint8_t> save_bytes;
            if (!read_file(args.save_path, &save_bytes, &err)) {
                std::fprintf(stderr, "[gbarecomp:runtime] %s\n", err.c_str());
                return 1;
            }
            if (save_bytes.size() > bus.save().eeprom_size()) {
                std::fprintf(stderr,
                             "[gbarecomp:runtime] save file too large: %s "
                             "(%zu bytes, EEPROM is %zu bytes)\n",
                             args.save_path.c_str(), save_bytes.size(),
                             bus.save().eeprom_size());
                return 1;
            }
            if (!bus.save().load_eeprom_bytes(save_bytes.data(),
                                              save_bytes.size())) {
                std::fprintf(stderr,
                             "[gbarecomp:runtime] failed to load EEPROM save "
                             "%s\n", args.save_path.c_str());
                return 1;
            }
            if (!args.quiet) {
                std::printf("save_loaded path=\"%s\" size=%zu/%zu\n",
                            args.save_path.c_str(), save_bytes.size(),
                            bus.save().eeprom_size());
            }
        }
    }
    bus.io().set_ppu(&ppu);
    bus.io().set_bus(&bus);

    auto flush_save = [&]() -> bool {
        if (!bus.save().eeprom_enabled() || args.save_path.empty() ||
            !bus.save().dirty()) {
            return true;
        }
        std::vector<uint8_t> save_bytes = bus.save().eeprom_bytes();
        if (!write_file(args.save_path, save_bytes, &err)) {
            std::fprintf(stderr, "[gbarecomp:runtime] %s\n", err.c_str());
            return false;
        }
        bus.save().clear_dirty();
        if (!args.quiet) {
            std::printf("save_flushed path=\"%s\" size=%zu\n",
                        args.save_path.c_str(), save_bytes.size());
        }
        return true;
    };

    set_active_bus(&bus);
    set_active_ppu(&ppu);
    runtime_init(&bus);
    reset_recomp_cpu();

    // ── Recompiler exec gate ──────────────────────────────────────
    //
    // The runtime is recompiler-driven (PRINCIPLES.md "Interpreter
    // is informative, never load-bearing"). step_once() calls
    // runtime_dispatch(PC) for whatever the recompiled CPU's PC
    // currently is. Today this fails immediately because:
    //   - BIOS code is not yet recompiled (no entry in the dispatch
    //     table for PC=0x00000000) → runtime_dispatch_miss aborts.
    //   - Cart code IS in the dispatch table, but every IrOp lowers
    //     to runtime_unimplemented_op pending Phase C codegen.
    // Either way the runtime aborts with a clear message naming the
    // gap. That abort is the gate; do NOT route to the interpreter.
    uint64_t taken = 0;
    uint64_t cycles_elapsed = 0;
    uint32_t last_step_cycles = 0;
    uint64_t vblank_count = 0;
    uint64_t vblank_irqs_raised = 0;
    uint64_t irq_entries = 0;
    uint64_t halt_steps = 0;
    uint64_t swi_entries = 0;
    int frames_presented = 0;
    bool host_quit = false;

    auto pump_idle = [&](uint32_t max_cycles) -> uint32_t {
        uint32_t chunk = ppu.cycles_until_next_event();
        uint32_t until_timer = bus.io().cycles_until_next_timer_event();
        uint32_t until_sample = bus.audio().cycles_until_next_sample();
        if (until_timer < chunk) chunk = until_timer;
        if (until_sample < chunk) chunk = until_sample;
        if (chunk == 0 || chunk == 0xFFFFFFFFu) chunk = 1;
        if (chunk > max_cycles) chunk = max_cycles;
        runtime_tick(chunk);
        cycles_elapsed += chunk;
        last_step_cycles += chunk;
        return chunk;
    };

    auto sync_frame_counter = [&]() {
        vblank_count = ppu.frame_count();
    };

    auto step_once = [&]() -> bool {
        last_step_cycles = 0;
        if (bus.io().halted()) {
            ++halt_steps;
            uint32_t idle_budget = gba::GbaPpu::kCyclesPerFrame;
            while (bus.io().halted() && idle_budget != 0) {
                uint32_t chunk = pump_idle(idle_budget);
                idle_budget -= chunk;
            }
            ++taken;
            sync_frame_counter();
            return true;
        }

        runtime_dispatch(g_cpu.R[15]);
        ++taken;
        sync_frame_counter();
        return true;
    };
    auto step_frame = [&]() -> bool {
        uint64_t start_frame = ppu.frame_count();
        constexpr uint64_t kMaxDispatchesPerFrame = 2'000'000ull;
        for (uint64_t i = 0; i < kMaxDispatchesPerFrame; ++i) {
            if (!step_once()) return false;
            if (ppu.frame_count() != start_frame) return true;
        }
        return false;
    };

    // ── Save-state hooks ───────────────────────────────────────────
    // Only ever invoked at a clean dispatch boundary: TCP commands run
    // between step calls, and the host-window hotkey is sampled at the
    // top of a loop iteration (never mid-runtime_dispatch). See
    // src/debug/snapshot.h for why that boundary is the only safe one.
    auto make_snapshot_ctx = [&]() {
        debug::SnapshotContext sc;
        sc.bus            = &bus;
        sc.ppu            = &ppu;
        sc.rom_sha1       = rom_sha1.c_str();
        sc.taken          = &taken;
        sc.cycles_elapsed = &cycles_elapsed;
        sc.vblank_count   = &vblank_count;
        return sc;
    };
    auto do_savestate_save = [&](const std::string& path,
                                 std::string& e) -> bool {
        return debug::save_state(path.c_str(), make_snapshot_ctx(), &e);
    };
    auto do_savestate_load = [&](const std::string& path,
                                 std::string& e) -> bool {
        if (!debug::load_state(path.c_str(), make_snapshot_ctx(), &e)) {
            return false;
        }
        sync_frame_counter();  // realign vblank_count with restored PPU
        return true;
    };

    if (args.tcp_port > 0) {
        debug::TcpDebugServer server;
        debug::TcpDebugServer::Context ctx;
        // ctx.cpu is the armv4t interpreter CPU type; the recomp
        // runtime exposes g_cpu through recomp_cpu instead.
        ctx.cpu = nullptr;
        ctx.recomp_cpu = &g_cpu;
        ctx.runtime_trace_copy = runtime_trace_copy_recent;
        ctx.bus = &bus;
        ctx.ppu = &ppu;
        ctx.step = step_frame;
        ctx.step_inst = step_once;
        ctx.savestate_save = do_savestate_save;
        ctx.savestate_load = do_savestate_load;
        ctx.irq_entries = &irq_entries;
        ctx.swi_entries = &swi_entries;
        ctx.halt_steps = &halt_steps;
        ctx.vblank_irqs_raised = &vblank_irqs_raised;
        ctx.steps = &taken;
        ctx.cycles_elapsed = &cycles_elapsed;
        ctx.last_step_cycles = &last_step_cycles;
        ctx.sync_frames = &vblank_count;
        bool ok = server.run(args.tcp_port, ctx);
        bool save_ok = flush_save();
        runtime_shutdown();
        return (ok && save_ok) ? 0 : 1;
    }

    HostWindow win;
    std::vector<uint8_t> live_fb;
    if (args.window) {
        if (!HostWindow::is_available()) {
            std::fprintf(stderr,
                         "[gbarecomp:runtime] --window requested but SDL2 "
                         "is not available in this build\n");
            runtime_shutdown();
            return 1;
        }
        if (!win.open(args.scale, GBARECOMP_WINDOW_TITLE)) {
            runtime_shutdown();
            return 1;
        }
        live_fb.assign(gba::GbaPpu::kFramebufferBytes, 0);
    }

    // Host-window save-state slots: the ROM path with a .stateN
    // extension (N = 1..9). Shift+Fn writes slot N, Fn restores it.
    auto slot_path = [&](int slot) -> std::string {
        std::filesystem::path p(args.rom);
        p.replace_extension(".state" + std::to_string(slot));
        return p.string();
    };

    uint64_t last_presented_frame = ppu.frame_count();

    auto pump_host_input = [&]() {
        if (!args.window) return;
        auto ev = win.pump();
        bus.io().set_keyinput(ev.keyinput);
        if (ev.quit) host_quit = true;
        if (ev.save_slot) {
            std::string path = slot_path(ev.save_slot);
            std::string e;
            if (do_savestate_save(path, e)) {
                std::printf("savestate_saved slot=%d path=\"%s\"\n",
                            ev.save_slot, path.c_str());
                std::fflush(stdout);
            } else {
                std::fprintf(stderr,
                             "[gbarecomp:runtime] savestate save (slot %d) "
                             "failed: %s\n", ev.save_slot, e.c_str());
            }
        }
        if (ev.load_slot) {
            std::string path = slot_path(ev.load_slot);
            std::string e;
            if (do_savestate_load(path, e)) {
                std::printf("savestate_loaded slot=%d path=\"%s\" pc=0x%08x "
                            "frame=%llu\n", ev.load_slot, path.c_str(),
                            g_cpu.R[15],
                            static_cast<unsigned long long>(ppu.frame_count()));
                std::fflush(stdout);
                // Force the next iteration to re-present the restored
                // frame instead of waiting for frame_count to advance.
                last_presented_frame = ppu.frame_count() - 1;
            } else {
                std::fprintf(stderr,
                             "[gbarecomp:runtime] savestate load (slot %d) "
                             "failed: %s\n", ev.load_slot, e.c_str());
            }
        }
    };

    const bool open_ended = (args.window || args.frames >= 0);
    const int step_budget = open_ended
        ? (args.steps > 16 ? args.steps : std::numeric_limits<int>::max() / 2)
        : args.steps;

    int dispatches_since_pump = 0;
    if (args.window) pump_host_input();

    for (int i = 0; i < step_budget && !host_quit; ++i) {
        if (!step_once()) break;
        if (args.window) {
            ++dispatches_since_pump;
            if (dispatches_since_pump >= 512) {
                pump_host_input();
                dispatches_since_pump = 0;
            }
            uint64_t frame = ppu.frame_count();
            if (frame != last_presented_frame) {
                if (ppu.has_latched_framebuffer()) {
                    std::memcpy(live_fb.data(), ppu.latched_framebuffer(),
                                gba::GbaPpu::kFramebufferBytes);
                } else {
                    ppu.render(live_fb.data(), bus.io().read16(0x000),
                               bus.io().raw(), bus.vram_ptr(), bus.oam_ptr(),
                               bus.pal_ptr());
                }
                win.present(live_fb.data());
                int16_t audio_buf[2048];
                std::size_t n = bus.audio().drain_samples(audio_buf, 2048);
                if (n > 0) win.push_audio_samples(audio_buf, n);
                pump_host_input();
                dispatches_since_pump = 0;
                last_presented_frame = frame;
                ++frames_presented;
                if (args.frames >= 0 && frames_presented >= args.frames) {
                    host_quit = true;
                }
            }
        }
        if (!args.window && args.frames >= 0 &&
            ppu.frame_count() >= static_cast<uint64_t>(args.frames)) {
            break;
        }
    }
    if (args.window) win.close();

    bool save_ok = flush_save();

    if (!args.dump_bmp.empty()) {
        std::vector<uint8_t> fb(gba::GbaPpu::kFramebufferBytes, 0);
        if (ppu.has_latched_framebuffer()) {
            std::memcpy(fb.data(), ppu.latched_framebuffer(), fb.size());
        } else {
            ppu.render(fb.data(), bus.io().read16(0x000), bus.io().raw(),
                       bus.vram_ptr(), bus.oam_ptr(), bus.pal_ptr());
        }
        if (!write_bmp(args.dump_bmp, fb.data(), gba::GbaPpu::kScreenWidth,
                       gba::GbaPpu::kScreenHeight)) {
            std::fprintf(stderr,
                         "[gbarecomp:runtime] failed to write %s\n",
                         args.dump_bmp.c_str());
        }
    }

    std::printf("\nfinal_pc=0x%08x unmapped=%zu io_unhandled=%zu "
                "steps=%llu cycles=%llu ppu_vcount=%u ppu_frames=%llu "
                "frames_presented=%d\n",
                g_cpu.R[15], bus.unmapped_count(),
                bus.io().unmapped_count(),
                static_cast<unsigned long long>(taken),
                static_cast<unsigned long long>(cycles_elapsed),
                static_cast<unsigned>(ppu.vcount()),
                static_cast<unsigned long long>(ppu.frame_count()),
                frames_presented);
    std::printf("pal_nonzero=%zu/1024 vram_nonzero=%zu/98304 "
                "oam_nonzero=%zu/1024\n",
                count_nonzero(bus.pal_ptr(), 1024),
                count_nonzero(bus.vram_ptr(), 96 * 1024),
                count_nonzero(bus.oam_ptr(), 1024));

    runtime_shutdown();
    return save_ok ? 0 : 1;
}

}  // namespace gbarecomp
