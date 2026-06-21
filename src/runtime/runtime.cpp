// runtime.cpp - shared BIOS+ROM runner for generated game binaries.
//
// This translation unit is itself interpreter-free: it initializes the
// recomp ABI state (g_cpu via runtime_arm.h) and drives execution by
// calling runtime_dispatch(pc). Two distinct gaps are handled very
// differently downstream (per PRINCIPLES.md "Honest self-healing"):
//   * a CODEGEN gap (unlowered IrOp) → runtime_unimplemented_op aborts
//     loudly; that abort is the gate, never papered over.
//   * a DISPATCH gap (undiscovered/excluded function) → runtime_dispatch_miss
//     SELF-HEALS: it bridges the call through the reference interpreter,
//     loudly logs it, and records it for a reviewed TOML proposal. The run
//     is reported as NOT fully static until coverage closes (see the
//     self-heal coverage banner emitted at exit).
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
#include "host_platform.h"
#include "host_window.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "self_heal.h"
#include "overlay_loader.h"
#include "sha1.h"
#include "snapshot.h"
#include "tcp_debug_server.h"
#include "ws_provenance.h"
#include "ws_sidecar.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Authoritative IRQ-entry counter, incremented in runtime_irq (runtime_arm.cpp)
// at the actual vectoring site. The TCP `counters` command surfaces it via
// ctx.irq_entries (the run-loop local never increments — IRQs are taken in
// runtime_tick, not here). See MC-HP-002 IRQ-delivery comparison.
extern "C" unsigned long long g_runtime_irq_entries;

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
    std::string dump_png;    // --dump-png: final framebuffer as PNG (preferred)
    std::string load_state;  // --load-state <path>: headless savestate load
    // [video] screen = raw|unlit|frontlit|backlit|classic — present-time
    // color simulation (see color_lut). Empty = raw (passthrough). The
    // GBARECOMP_SCREEN env var overrides this at launch.
    std::string screen;
    // [audio] shadow = true|false — arm the MP2K verified-enhancement shadow
    // mixer (default off). GBARECOMP_AUDIO_SHADOW overrides at launch.
    bool audio_shadow = false;
    // --widescreen <N> / [video].widescreen: opt-in view-area expansion. N =
    // extra columns rendered per side (left & right); 0 = OFF = byte-identical
    // to the faithful 240x160 build. Vertical expansion is deferred, so only
    // the horizontal margin is exposed. Clamped to GbaPpu::kMaxExtraX. The
    // GBARECOMP_WIDESCREEN env var overrides at launch.
    int widescreen = 0;
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

// Minimal, dependency-free PNG writer (8-bit RGB). Uses stored/uncompressed
// DEFLATE blocks so we need no zlib — just a local CRC-32 (PNG/zlib IEEE
// poly) and Adler-32. Mirrors the spirit of write_bmp; preferred output for
// visual inspection since the Read tool renders PNG but not BMP.
bool write_png(const std::string& path, const uint8_t* rgb,
               uint32_t w, uint32_t h) {
    auto crc32 = [](const uint8_t* p, std::size_t n, uint32_t crc) -> uint32_t {
        crc = ~crc;
        for (std::size_t i = 0; i < n; ++i) {
            crc ^= p[i];
            for (int k = 0; k < 8; ++k)
                crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
        }
        return ~crc;
    };
    std::vector<uint8_t> out;
    auto put_be32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v >> 24));
        out.push_back(static_cast<uint8_t>(v >> 16));
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v));
    };
    auto chunk = [&](const char tag[4], const std::vector<uint8_t>& data) {
        put_be32(static_cast<uint32_t>(data.size()));
        std::size_t type_at = out.size();
        out.insert(out.end(), tag, tag + 4);
        out.insert(out.end(), data.begin(), data.end());
        out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0);
        uint32_t c = crc32(out.data() + type_at, 4 + data.size(), 0);
        out[out.size() - 4] = static_cast<uint8_t>(c >> 24);
        out[out.size() - 3] = static_cast<uint8_t>(c >> 16);
        out[out.size() - 2] = static_cast<uint8_t>(c >> 8);
        out[out.size() - 1] = static_cast<uint8_t>(c);
    };

    // PNG signature.
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), sig, sig + 8);

    // IHDR: width, height, bit depth 8, color type 2 (truecolor RGB).
    std::vector<uint8_t> ihdr;
    auto ihdr_be32 = [&](uint32_t v) {
        ihdr.push_back(static_cast<uint8_t>(v >> 24));
        ihdr.push_back(static_cast<uint8_t>(v >> 16));
        ihdr.push_back(static_cast<uint8_t>(v >> 8));
        ihdr.push_back(static_cast<uint8_t>(v));
    };
    ihdr_be32(w); ihdr_be32(h);
    ihdr.push_back(8); ihdr.push_back(2);
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk("IHDR", ihdr);

    // Raw filtered scanlines: each row prefixed with filter byte 0 (None).
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (1 + static_cast<std::size_t>(w) * 3));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + static_cast<std::size_t>(y) * w * 3,
                   rgb + static_cast<std::size_t>(y + 1) * w * 3);
    }

    // zlib stream: 2-byte header + stored DEFLATE blocks + Adler-32 of raw.
    std::vector<uint8_t> zlib;
    zlib.push_back(0x78); zlib.push_back(0x01);
    std::size_t off = 0;
    while (off < raw.size() || raw.empty()) {
        std::size_t n = std::min<std::size_t>(raw.size() - off, 65535);
        bool final = (off + n >= raw.size());
        zlib.push_back(final ? 1 : 0);
        zlib.push_back(static_cast<uint8_t>(n & 0xFF));
        zlib.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        zlib.push_back(static_cast<uint8_t>(~n & 0xFF));
        zlib.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
        if (final) break;
    }
    uint32_t a1 = 1, a2 = 0;
    for (uint8_t b : raw) { a1 = (a1 + b) % 65521; a2 = (a2 + a1) % 65521; }
    uint32_t adler = (a2 << 16) | a1;
    zlib.push_back(static_cast<uint8_t>(adler >> 24));
    zlib.push_back(static_cast<uint8_t>(adler >> 16));
    zlib.push_back(static_cast<uint8_t>(adler >> 8));
    zlib.push_back(static_cast<uint8_t>(adler));
    chunk("IDAT", zlib);
    chunk("IEND", {});

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()),
            static_cast<std::streamsize>(out.size()));
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
        } else if (section == "video" && key == "screen") {
            args->screen = val;
        } else if (section == "video" && key == "widescreen") {
            int n = 0;
            if (parse_int(val.c_str(), &n) && n >= 0) args->widescreen = n;
        } else if (section == "audio" && key == "shadow") {
            args->audio_shadow = (val == "true" || val == "1");
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
             s == "--dump-png" || s == "--load-state" ||
             s == "--widescreen" ||
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
        if (s == "--dump-png") {
            const char* v = need_value("--dump-png");
            if (!v) return false;
            args->dump_png = v;
            continue;
        }
        if (s == "--load-state") {
            const char* v = need_value("--load-state");
            if (!v) return false;
            args->load_state = v;
            continue;
        }
        if (s == "--widescreen") {
            const char* v = need_value("--widescreen");
            if (!v) return false;
            if (!parse_int(v, &args->widescreen) || args->widescreen < 0) {
                if (err) *err = "invalid --widescreen value (expected >= 0)";
                return false;
            }
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
    for (int i = 0; i < ARM_BANK_COUNT; ++i) {
        g_cpu.banked_sp[i] = 0;
        g_cpu.banked_lr[i] = 0;
        g_cpu.banked_spsr[i] = 0;
    }
    for (int i = 0; i < 5; ++i) { g_cpu.r8_12_user[i] = 0; g_cpu.r8_12_fiq[i] = 0; }
    g_cpu.R[13] = 0x03007FE0;
    g_cpu.cpsr = CPSR_I_BIT | CPSR_F_BIT | 0x13u /* SVC */;
    // Seed the banked stack pointers to the canonical GBA post-reset values
    // (GBATEK "GBA Reset"; what hardware / mGBA / the bios_smoke interpreter
    // oracle leave after BIOS reset). Without this the User/System and IRQ banks
    // were 0, so the BIOS reset path's first `msr cpsr,#0x1f` (System mode, at
    // BIOS 0x90) banked in SP=0 instead of 0x03007F00 — the first recomp-vs-interp
    // divergence (cycle 16), cascading into stack writes to address ~0 and a
    // multi-KB IWRAM divergence. (MC-HP-002 fresh-boot root.)
    g_cpu.banked_sp[ARM_BANK_SUPERVISOR] = 0x03007FE0;
    g_cpu.banked_sp[ARM_BANK_IRQ]        = 0x03007FA0;
    g_cpu.banked_sp[ARM_BANK_USER]       = 0x03007F00;
}

std::size_t count_nonzero(const uint8_t* p, std::size_t n) {
    std::size_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != 0) ++c;
    }
    return c;
}

}  // namespace

// Opt-in view-area expansion master switch + parameter (the runner owns
// enhancement policy). The future per-game widescreen injector externs these
// from generated cart code to gate its map/camera widening on g_ws_active and
// scale by g_ws_extra. 0/0 = faithful 240x160; set once from --widescreen.
extern "C" unsigned g_ws_active = 0;
extern "C" unsigned g_ws_extra  = 0;

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
    bus.request_audio_shadow(args.audio_shadow);  // [audio].shadow default; env can override

    // View-area expansion (opt-in enhancement). GBARECOMP_WIDESCREEN overrides
    // the --widescreen / [video].widescreen value at launch (matches the
    // screen/shadow override convention). N = extra columns per side; 0 = the
    // faithful 240x160 view (byte-identical). The PPU clamps to kMaxExtraX.
    {
        int ws = args.widescreen;
        if (const char* e = std::getenv("GBARECOMP_WIDESCREEN")) {
            int n = 0;
            if (parse_int(e, &n) && n >= 0) ws = n;
        }
        ppu.set_view_margins(static_cast<uint32_t>(ws),
                             static_cast<uint32_t>(ws), 0, 0);
        g_ws_extra  = static_cast<unsigned>(ppu.view_extra_left());
        g_ws_active = ppu.view_expanded() ? 1u : 0u;
        if (ppu.view_expanded() && !args.quiet) {
            std::fprintf(stderr,
                "[gbarecomp:runtime] view-area expansion ON: +%u px/side "
                "(render %ux%u); set --widescreen 0 for the faithful view\n",
                static_cast<unsigned>(ppu.view_extra_left()),
                ppu.render_width(), ppu.render_height());
        }
    }
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
    self_heal_reset();  // fresh coverage tally for this machine bring-up
    // Stage-2 self-heal: wire the on-the-fly recompiler (gated behind
    // GBARECOMP_SELFHEAL_RECOMPILE; no-op + a clear banner when off). The cache
    // is keyed by the cart SHA-1 so a different ROM gets a fresh DLL set; the
    // BIOS snapshot is the immutable code image for BIOS-region heals.
    gbarecomp::overlay_loader_init("recomp_cache", rom_sha1, &bios);

    // Widescreen Step B tilemap-provenance probe (debug-only; no-op unless
    // GBARECOMP_WS_PROBE_DRAWMETATILE is set). Installs the function-entry hook
    // so the owner ring tracks DrawMetatileAt continuously from boot.
    ws_provenance_init_from_env();
    ws_sidecar_init_from_env();   // widescreen Step C margin-injection sidecar

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

    // Emitted once at shutdown on every exit path: the self-heal coverage
    // banner (+ reviewed TOML proposal) and, when GBARECOMP_FP_SAVE names a
    // path, a dump of the always-on per-instruction fingerprint ring. The fp
    // ring records from machine reset (no arm-then-run gap), so this is a
    // pure query of recorded history — two builds run identically diff
    // bit-for-bit on these files. (Arm with GBARECOMP_INSN_TRACE=1.)
    auto emit_exit_diagnostics = [&]() {
        const char* frag = std::getenv("GBARECOMP_MISS_FRAG");
        gbarecomp::self_heal_write_report(
            frag ? frag : "recomp_master_misses.toml.frag");
        // Machine-readable coverage summary for the build loop / CI (written
        // every exit, FULLY_STATIC included). Override path with env.
        const char* cov = std::getenv("GBARECOMP_COVERAGE_JSON");
        gbarecomp::self_heal_write_coverage_json(
            cov ? cov : "recomp_coverage.json");
        if (const char* fp = std::getenv("GBARECOMP_FP_SAVE")) {
            uint32_t n = runtime_fp_save_file(fp);
            std::printf("fp_ring_saved path=\"%s\" records=%u\n", fp, n);
        }
        if (const char* il = std::getenv("GBARECOMP_IRQ_LOG")) {
            uint32_t n = runtime_irq_log_save_file(il);
            std::printf("irq_log_saved path=\"%s\" records=%u\n", il, n);
        }
        if (const char* sl = std::getenv("GBARECOMP_SWI_LOG")) {
            uint32_t n = runtime_swi_log_save_file(sl);
            std::printf("swi_log_saved path=\"%s\" records=%u\n", sl, n);
        }
    };

    auto step_once = [&]() -> bool {
        last_step_cycles = 0;
        // Game-thread-only: fold any worker-finished native overlays into the
        // dispatch table so the next runtime_dispatch can use them. Cheap when
        // idle (a single atomic load); see overlay_loader.cpp.
        gbarecomp::overlay_drain_ready();
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
    // Advance until the PPU reaches the next VBlank-start (scanline
    // 159->160), NOT the scanline wrap (227->0). The interpreter oracle
    // (tools/bios_smoke step_one_frame) and mGBA (runFrame) both return at
    // VBlank-start; keying off ppu.frame_count() (which increments at the
    // wrap) parked the recomp ~68 scanlines / one frame of game-logic later
    // than the oracles, so memory diffed at the same step index showed a
    // spurious "recomp is a frame ahead". g_runtime_vblank_starts counts
    // VBlank-start events; stopping on its increment puts the recomp at the
    // same PPU phase as both oracles. See runtime_bus_bridge.cpp.
    auto step_frame = [&]() -> bool {
        uint64_t start_vbl = g_runtime_vblank_starts;
        constexpr uint64_t kMaxDispatchesPerFrame = 2'000'000ull;
        for (uint64_t i = 0; i < kMaxDispatchesPerFrame; ++i) {
            if (!step_once()) return false;
            if (g_runtime_vblank_starts != start_vbl) return true;
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
        // Re-origin the fingerprint cycle clock + ring at the load point so the
        // recomp and interp oracle share a cycle origin for diff_cycle_trace.py.
        // (The snapshot's absolute cycle count is irrelevant; we diff relative to
        // the load. The interp re-origins its own cycles_elapsed identically.)
        g_runtime_cycles = 0;
        runtime_fp_reset();
        return true;
    };

    // ── Differential-oracle WRAM trace (gbaref/snesref/mdref method) ──
    // GBARECOMP_WRAM_TRACE=path emits one JSONL record per changed work-RAM byte
    // per frame — the SAME shape gbaref (the libretro GBA reference) writes — so
    // oracle/ref_diff.py can diff the recompiled run against a known-good core
    // WITHOUT savestate alignment or scripted boot (diff by value/order, not
    // frame). Defined BEFORE the TCP block so it fires in ALL drive modes — TCP
    // (each step), windowed play, and headless — at full region parity. The
    // interpreter (bios_smoke) shares src/gba/* device models with this runtime
    // so it can't arbitrate device/bus bugs — gbaref can. Range-limit with
    // GBARECOMP_WRAM_TRACE_LO/_HI (GBA absolute addresses).
    std::FILE* wram_trace_log = nullptr;
    uint32_t wram_trace_lo = 0x00000000u, wram_trace_hi = 0xFFFFFFFFu;
    bool wram_trace_primed = false;
    uint64_t wram_trace_frame = 0;
    uint64_t wram_last_traced = ppu.frame_count();
    // Full-parity region set: every WRITABLE GBA region mGBA exposes via its
    // libretro memory map, so gbaref and this trace cover the same address space
    // (BIOS/ROM are const — skipped). base, host ptr, length, prev-frame shadow.
    struct TraceRegion { uint32_t base; const uint8_t* ptr; std::size_t len;
                         std::vector<uint8_t> prev; };
    std::vector<TraceRegion> wram_regions;
    if (const char* wp = std::getenv("GBARECOMP_WRAM_TRACE")) {
        wram_trace_log = std::fopen(wp[0] ? wp : "gbarecomp_trace.jsonl", "w");
        if (const char* lo = std::getenv("GBARECOMP_WRAM_TRACE_LO"))
            wram_trace_lo = static_cast<uint32_t>(std::strtoul(lo, nullptr, 0));
        if (const char* hi = std::getenv("GBARECOMP_WRAM_TRACE_HI"))
            wram_trace_hi = static_cast<uint32_t>(std::strtoul(hi, nullptr, 0));
        auto reg = [&](uint32_t base, const uint8_t* ptr, std::size_t len) {
            // Only materialize regions overlapping [lo,hi] to keep traces small.
            if (base > wram_trace_hi || base + len <= wram_trace_lo) return;
            wram_regions.push_back({base, ptr, len, std::vector<uint8_t>(len, 0)});
        };
        reg(0x02000000u, bus.ewram_ptr(),   256 * 1024);
        reg(0x03000000u, bus.iwram_ptr(),    32 * 1024);
        reg(0x04000000u, bus.io().raw(),     gba::GbaIo::kIoSize);
        reg(0x05000000u, bus.pal_ptr(),       1 * 1024);
        reg(0x06000000u, bus.vram_ptr(),     96 * 1024);
        reg(0x07000000u, bus.oam_ptr(),       1 * 1024);
    }
    auto wram_trace_tick = [&]() {
        if (!wram_trace_log) return;
        for (auto& rg : wram_regions) {
            for (std::size_t i = 0; i < rg.len; ++i) {
                uint8_t v = rg.ptr[i];
                if (v != rg.prev[i]) {
                    uint32_t a = rg.base + static_cast<uint32_t>(i);
                    if (wram_trace_primed && a >= wram_trace_lo && a <= wram_trace_hi)
                        std::fprintf(wram_trace_log,
                            "{\"f\":%llu,\"adr\":\"0x%08x\",\"old\":\"0x%02x\","
                            "\"val\":\"0x%02x\"}\n",
                            static_cast<unsigned long long>(wram_trace_frame),
                            a, rg.prev[i], v);
                    rg.prev[i] = v;
                }
            }
        }
        wram_trace_primed = true;
        ++wram_trace_frame;
        // Flush every frame: when the bug HANGS, no further frame presents, so
        // the process is killed — the last pre-hang frame must already be on disk.
        std::fflush(wram_trace_log);
    };

    if (args.tcp_port > 0) {
        // ── Free-run threading model ───────────────────────────────────────
        // The game CORE runs on a dedicated thread; the TCP server runs on THIS
        // (main) thread. Synchronous `step` (oracle lockstep) signals the game
        // thread and waits — same observable behavior as before, so existing
        // diff/lockstep scripts are unaffected. `continue` lets the game
        // free-run, which frees the server thread to answer OBSERVATION commands
        // (registers / read_* / symbol / misses) WHILE the game thread is wedged
        // in a busy-spin freeze — a hung core stays fully introspectable (the
        // MC-HP-002 need; before this, a never-returning step took the whole
        // debugger down with it). Observation reads g_cpu/bus racily: aligned
        // 32-bit reads are tear-free on x86, and the GBARECOMP_SAMPLE sampler
        // already relies on exactly this. Mutating ops (savestate/load) require
        // the game PARKED at a frame boundary (the only place the host
        // call-return stack is consistent) — enforced by park_and_wait below.
        enum RunState { RS_PAUSED = 0, RS_RUNNING = 1, RS_STEP = 2, RS_QUIT = 3 };
        std::mutex              ctl_m;
        std::condition_variable ctl_cv;
        int  ctl_state     = RS_PAUSED;
        bool ctl_parked    = true;
        bool ctl_step_done = false;
        bool ctl_step_ok   = true;

        std::thread game_thread([&]() {
            for (;;) {
                int st;
                {
                    std::unique_lock<std::mutex> lk(ctl_m);
                    ctl_parked = (ctl_state == RS_PAUSED);
                    if (ctl_parked) ctl_cv.notify_all();
                    ctl_cv.wait_for(lk, std::chrono::milliseconds(4),
                                    [&] { return ctl_state != RS_PAUSED; });
                    st = ctl_state;
                }
                if (st == RS_QUIT) break;
                if (st == RS_PAUSED) continue;
                // RS_RUNNING / RS_STEP: run one frame. May wedge inside a freeze
                // (step_frame never returns); the server thread stays live for
                // observation regardless. set_keyinput writes bus.io directly and
                // persists, so input set before `continue` holds across frames.
                bool ok = step_frame();
                if (ok) wram_trace_tick();
                std::lock_guard<std::mutex> lk(ctl_m);
                if (st == RS_STEP) {
                    ctl_step_ok   = ok;
                    ctl_step_done = true;
                    ctl_state     = RS_PAUSED;
                    ctl_cv.notify_all();
                } else if (!ok) {           // RUNNING hit an abnormal stop → park
                    ctl_state = RS_PAUSED;
                    ctl_cv.notify_all();
                }
            }
        });

        // Park the game at a frame boundary (best-effort, timeout). Returns true
        // if parked; false if the core is wedged/running past the timeout (then a
        // mutating op must refuse — you cannot snapshot a mid-dispatch hung core).
        auto park_and_wait = [&](int timeout_ms) -> bool {
            std::unique_lock<std::mutex> lk(ctl_m);
            if (ctl_state == RS_RUNNING) ctl_state = RS_PAUSED;
            ctl_cv.notify_all();
            return ctl_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                   [&] { return ctl_parked; });
        };

        debug::TcpDebugServer server;
        debug::TcpDebugServer::Context ctx;
        // ctx.cpu is the armv4t interpreter CPU type; the recomp
        // runtime exposes g_cpu through recomp_cpu instead.
        ctx.cpu = nullptr;
        ctx.recomp_cpu = &g_cpu;
        ctx.runtime_trace_copy = runtime_trace_copy_recent;
        ctx.bus = &bus;
        ctx.ppu = &ppu;
        // Synchronous step: drive ONE frame on the game thread and wait for it.
        // Same observable semantics as the old in-line step (the WRAM trace fires
        // on the game thread inside step). Oracle lockstep scripts are unaffected.
        ctx.step = [&]() -> bool {
            std::unique_lock<std::mutex> lk(ctl_m);
            ctl_step_done = false;
            ctl_state     = RS_STEP;
            ctl_cv.notify_all();
            ctl_cv.wait(lk, [&] { return ctl_step_done; });
            return ctl_step_ok;
        };
        ctx.step_inst = step_once;
        ctx.savestate_save = [&](const std::string& p, std::string& e) -> bool {
            if (!park_and_wait(2000)) {
                e = "core not parked (running/wedged) — pause first";
                return false;
            }
            return do_savestate_save(p, e);
        };
        ctx.savestate_load = [&](const std::string& p, std::string& e) -> bool {
            if (!park_and_wait(2000)) {
                e = "core not parked (running/wedged) — pause first";
                return false;
            }
            return do_savestate_load(p, e);
        };
        ctx.fp_save = [](const std::string& p) {
            return runtime_fp_save_file(p.c_str());
        };
        ctx.misses_query = []() {
            return gbarecomp::self_heal_misses_json();
        };
        ctx.resume = [&]() {
            std::lock_guard<std::mutex> lk(ctl_m);
            ctl_state = RS_RUNNING;
            ctl_cv.notify_all();
        };
        ctx.pause = [&]() { park_and_wait(2000); };
        ctx.run_status = [&]() -> std::string {
            int st; bool pk;
            { std::lock_guard<std::mutex> lk(ctl_m); st = ctl_state; pk = ctl_parked; }
            const char* name = st == RS_RUNNING ? "running"
                             : st == RS_STEP    ? "stepping"
                             : st == RS_QUIT    ? "quit" : "paused";
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"run\":\"%s\",\"parked\":%s,\"pc\":\"0x%08X\","
                "\"cpsr\":\"0x%08X\",\"frame\":%llu,\"vblank_starts\":%llu}",
                name, pk ? "true" : "false",
                static_cast<unsigned>(g_cpu.R[15]),
                static_cast<unsigned>(g_cpu.cpsr),
                static_cast<unsigned long long>(ppu.frame_count()),
                static_cast<unsigned long long>(g_runtime_vblank_starts));
            return std::string(buf);
        };
        // The recomp vectors IRQs in runtime_irq (runtime_arm.cpp), called from
        // runtime_tick — NOT in this run loop — so the local irq_entries never
        // increments. Surface the authoritative global counter from the actual
        // delivery site instead (MC-HP-002 IRQ-delivery comparison).
        (void)irq_entries;
        ctx.irq_entries = reinterpret_cast<uint64_t*>(&g_runtime_irq_entries);
        ctx.swi_entries = &swi_entries;
        ctx.halt_steps = &halt_steps;
        ctx.vblank_irqs_raised = &vblank_irqs_raised;
        ctx.steps = &taken;
        // The recomp's authoritative cycle clock is g_runtime_cycles (ticked in
        // runtime_tick, used by the fp ring) — NOT the local `cycles_elapsed`,
        // which only grows in the HALT idle pump (pump_idle) and stays ~0 on the
        // TCP dispatch path. Surface the real clock so cross-engine cycle diffs
        // (oracle/cycle_onset.py) align with the interp's cycles_elapsed.
        // (The local `cycles_elapsed` is still used by the save-state ctx.)
        ctx.cycles_elapsed = reinterpret_cast<uint64_t*>(&g_runtime_cycles);
        ctx.last_step_cycles = &last_step_cycles;
        ctx.sync_frames = &vblank_count;
        bool ok = server.run(args.tcp_port, ctx);
        // Server returned (client sent `quit`). Stop the game thread. If it is
        // wedged in a freeze it will not observe RS_QUIT until the frame returns
        // (it never will) — but a clean quit is always issued while parked, so
        // join() returns; a wedged session is ended with taskkill instead.
        {
            std::lock_guard<std::mutex> lk(ctl_m);
            ctl_state = RS_QUIT;
            ctl_cv.notify_all();
        }
        if (game_thread.joinable()) game_thread.join();
        bool save_ok = flush_save();
        gbarecomp::overlay_loader_shutdown();  // join worker + drain before banner
        emit_exit_diagnostics();
        runtime_shutdown();
        return (ok && save_ok) ? 0 : 1;
    }

    HostWindow win;
    std::vector<uint8_t> live_fb;
    // Wall-clock frame limiter — constructed only for windowed runs so
    // the Windows timer-resolution bump doesn't apply to headless/TCP
    // batch runs (which intentionally run uncapped).
    std::optional<FramePacer> pacer;
    if (args.window) {
        if (!HostWindow::is_available()) {
            std::fprintf(stderr,
                         "[gbarecomp:runtime] --window requested but SDL2 "
                         "is not available in this build\n");
            gbarecomp::overlay_loader_shutdown();
            runtime_shutdown();
            return 1;
        }
        if (!win.open(args.scale, ppu.render_width(), ppu.render_height(),
                      GBARECOMP_WINDOW_TITLE,
                      args.screen.empty() ? nullptr : args.screen.c_str())) {
            gbarecomp::overlay_loader_shutdown();
            runtime_shutdown();
            return 1;
        }
        live_fb.assign(ppu.render_bytes(), 0);
        pacer.emplace();  // paces to the GBA's 59.7275 Hz
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
        if (pacer) pacer->set_uncapped(ev.fast_forward);
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
                // The load jumped guest time; realign the pacer so it
                // doesn't burn the accumulated lag catching up.
                if (pacer) pacer->reset();
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

    // Headless savestate load (--load-state <path>): boot fresh from the BIOS
    // reset, then restore a saved gameplay state before stepping. Lets a
    // non-windowed run jump straight to gameplay (e.g. to exercise a mid-run
    // self-heal at a deep-gameplay finder gap) without driving blind input
    // through the intro. do_savestate_load realigns the frame counter and
    // re-origins the fingerprint clock at the load point.
    if (!args.load_state.empty()) {
        std::string e;
        if (do_savestate_load(args.load_state, e)) {
            std::printf("savestate_loaded path=\"%s\" pc=0x%08x frame=%llu\n",
                        args.load_state.c_str(), g_cpu.R[15],
                        static_cast<unsigned long long>(ppu.frame_count()));
            std::fflush(stdout);
        } else {
            std::fprintf(stderr,
                         "[gbarecomp:runtime] --load-state \"%s\" failed: %s\n",
                         args.load_state.c_str(), e.c_str());
            gbarecomp::overlay_loader_shutdown();
            runtime_shutdown();
            return 1;
        }
    }

    // Deterministic demo-input driver for headless runs (opt-in via
    // GBARECOMP_DEMO_INPUT). A no-input headless run idles on the
    // title/file-select screen and never reaches the indirect-dispatch-
    // heavy gameplay code (e.g. the genuine finder gaps the self-heal
    // surfaces). This synthesizes a reproducible, frame-indexed button
    // track — each button held 6 frames then released 6 (edge-triggered
    // menus need the release) — cycling Start/A to clear menus + select
    // the loaded save, and directions/B to move + interact in-world.
    // Ported from tools/bios_smoke (Stage 0.2) so the input is identical
    // across the interpreter profiler and this recompiled runtime. Has no
    // effect on --window (host keyboard wins) or verify/oracle runs.
    // GBARECOMP_DEMO_INPUT modes: unset = off; "walk" = sustained-direction
    // overworld walker (crosses screen boundaries to surface transition-time
    // finder gaps); any other value = the menu/masher track.
    const char* demo_env = std::getenv("GBARECOMP_DEMO_INPUT");
    const bool demo_input = !args.window && demo_env != nullptr;
    const bool demo_walk = demo_env && std::strcmp(demo_env, "walk") == 0;
    auto demo_keyinput_for_frame = [](uint64_t frame) -> uint16_t {
        enum : uint16_t {
            KEY_A = 1u << 0, KEY_B = 1u << 1, KEY_START = 1u << 3,
            KEY_RIGHT = 1u << 4, KEY_LEFT = 1u << 5,
            KEY_UP = 1u << 6, KEY_DOWN = 1u << 7,
        };
        static const uint16_t kButtons[] = {
            KEY_START, KEY_A, KEY_A, KEY_DOWN, KEY_A, KEY_RIGHT,
            KEY_B, KEY_A, KEY_LEFT, KEY_A, KEY_UP, KEY_B,
        };
        if (((frame / 6) & 1u) != 0u) return 0x03FFu;  // release phase
        const uint16_t btn = kButtons[(frame / 12) %
            (sizeof(kButtons) / sizeof(kButtons[0]))];
        return static_cast<uint16_t>(0x03FFu & ~btn);  // KEYINPUT active-low
    };
    // Walk track: HOLD one direction long enough to cross a screen (~180
    // frames at ~1px/frame over a 240px screen), cycling R/D/L/U so the
    // player wanders through screen transitions in every direction. A brief
    // A tap every 60 frames dismisses incidental text / transition prompts.
    auto walk_keyinput_for_frame = [](uint64_t frame) -> uint16_t {
        enum : uint16_t {
            KEY_A = 1u << 0, KEY_RIGHT = 1u << 4, KEY_LEFT = 1u << 5,
            KEY_UP = 1u << 6, KEY_DOWN = 1u << 7,
        };
        static const uint16_t kDirs[] = { KEY_RIGHT, KEY_DOWN, KEY_LEFT, KEY_UP };
        constexpr uint64_t kHold = 180;
        uint16_t btn = kDirs[(frame / kHold) % 4];
        if ((frame % 60) < 2) btn |= KEY_A;
        return static_cast<uint16_t>(0x03FFu & ~btn);  // KEYINPUT active-low
    };
    uint64_t demo_last_frame = ~uint64_t{0};
    uint64_t sc_last_frame = ~uint64_t{0};  // widescreen sidecar per-frame sync

    // Headless --frames is a count of frames to run from where we start, not
    // an absolute PPU frame index — so a --load-state run executes args.frames
    // more frames past the restored frame counter instead of breaking on the
    // first step when the loaded index already exceeds the cap.
    const uint64_t headless_base_frame = ppu.frame_count();

    int dispatches_since_pump = 0;
    if (args.window) pump_host_input();

    for (int i = 0; i < step_budget && !host_quit; ++i) {
        if (!step_once()) break;
        if (demo_input) {
            const uint64_t fc = ppu.frame_count();
            if (fc != demo_last_frame) {
                demo_last_frame = fc;
                bus.io().set_keyinput(demo_walk ? walk_keyinput_for_frame(fc)
                                                : demo_keyinput_for_frame(fc));
            }
        }
        // Widescreen sidecar: capture the live tilemap ring once per guest
        // frame (no-op unless armed) so resident tiles are cached before the
        // camera evicts them — the always-on substrate for margin injection.
        if (ws_sidecar_enabled()) {
            const uint64_t scf = ppu.frame_count();
            if (scf != sc_last_frame) {
                sc_last_frame = scf;
                // Active mode: render the extended world (incl. never-seen
                // margins) via the guest's own draw — cycle/IRQ-transparent.
                // Only when widescreen is actually on (no point, and avoids any
                // synthetic-draw side effects, when off). Otherwise capture the
                // live ring (eviction).
                if (ws_sidecar_active_mode() && args.widescreen > 0)
                    ws_sidecar_active_fill();
                else if (!ws_sidecar_active_mode())
                    ws_sidecar_sync_frame();
            }
        }
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
                                ppu.render_bytes());
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
                // Pace to real GBA time. Decoupled from monitor vsync
                // (MC-HP-004); hold Tab to uncap (fast-forward).
                if (pacer) pacer->wait_for_next_frame();
            }
        }
        // Differential-oracle WRAM trace fires on frame advance in BOTH windowed
        // and headless modes (the present block above only runs windowed).
        if (wram_trace_log) {
            uint64_t fc_now = ppu.frame_count();
            if (fc_now != wram_last_traced) {
                wram_last_traced = fc_now;
                wram_trace_tick();
            }
        }
        if (!args.window && args.frames >= 0 &&
            ppu.frame_count() - headless_base_frame >=
                static_cast<uint64_t>(args.frames)) {
            break;
        }
    }
    if (args.window) win.close();

    bool save_ok = flush_save();

    if (!args.dump_bmp.empty() || !args.dump_png.empty()) {
        std::vector<uint8_t> fb(ppu.render_bytes(), 0);
        // Widescreen sidecar: populate the extended tilemap (incl. never-seen
        // margins) via the guest's own draw, then force a FRESH render so the
        // wide path reads the filled cache instead of the run-time latched FB.
        bool fresh = false;
        if (ws_sidecar_enabled() && args.widescreen > 0 &&
            ws_sidecar_active_mode()) {
            ws_sidecar_active_fill();
            fresh = true;
        }
        if (ppu.has_latched_framebuffer() && !fresh) {
            std::memcpy(fb.data(), ppu.latched_framebuffer(), fb.size());
        } else {
            ppu.render(fb.data(), bus.io().read16(0x000), bus.io().raw(),
                       bus.vram_ptr(), bus.oam_ptr(), bus.pal_ptr());
        }
        if (!args.dump_bmp.empty() &&
            !write_bmp(args.dump_bmp, fb.data(), ppu.render_width(),
                       ppu.render_height())) {
            std::fprintf(stderr,
                         "[gbarecomp:runtime] failed to write %s\n",
                         args.dump_bmp.c_str());
        }
        if (!args.dump_png.empty() &&
            !write_png(args.dump_png, fb.data(), ppu.render_width(),
                       ppu.render_height())) {
            std::fprintf(stderr,
                         "[gbarecomp:runtime] failed to write %s\n",
                         args.dump_png.c_str());
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

    // Widescreen provenance dump (debug-only): query the always-on owner ring
    // for the final guest state. GBARECOMP_WS_PROBE_DUMP=<path>.
    if (ws_provenance_armed()) {
        if (const char* wsp = std::getenv("GBARECOMP_WS_PROBE_DUMP")) {
            if (wsp[0]) ws_provenance_dump(wsp, args.widescreen);
        }
    }
    if (ws_sidecar_enabled()) {
        if (const char* scp = std::getenv("GBARECOMP_WS_SC_DUMP")) {
            if (scp[0]) ws_sidecar_dump(scp, args.widescreen);
        }
    }

    gbarecomp::overlay_loader_shutdown();  // join worker + drain before banner
    emit_exit_diagnostics();
    runtime_shutdown();
    return save_ok ? 0 : 1;
}

}  // namespace gbarecomp
