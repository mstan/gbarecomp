// gba_scan — read a GBA ROM, parse its header, identify the save
// chip, and print a structured report. The recompiler uses this
// upstream of `gba_recompile` to confirm the ROM matches what
// `game.toml` claims.
//
// Output format is intentionally machine-friendly (one `key=value`
// per line) so other tools — including the runner's pre-flight
// hash/identity check — can parse it without a JSON dep yet.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gba_rom_header.h"

namespace {

std::vector<uint8_t> read_file(const char* path, std::string* err) {
    std::vector<uint8_t> out;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { *err = std::string("open failed: ") + path; return out; }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { *err = "empty or unseekable"; std::fclose(f); return out; }
    out.resize(static_cast<std::size_t>(sz));
    std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        *err = "short read";
        out.clear();
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: gba_scan <rom_path>\n");
        return 2;
    }
    std::string err;
    auto rom = read_file(argv[1], &err);
    if (rom.empty()) {
        std::printf("error: %s\n", err.c_str());
        return 1;
    }

    auto h = gba::parse_rom(rom.data(), rom.size());

    std::printf("rom_path=%s\n", argv[1]);
    std::printf("rom_size=0x%zx\n", rom.size());
    std::printf("entry_branch_word=0x%08x\n", h.entry_branch_word);
    std::printf("entry_is_branch=%d\n", h.entry_is_branch ? 1 : 0);
    std::printf("entry_target=0x%08x\n", h.entry_target);
    std::printf("game_title=%s\n", h.game_title.c_str());
    std::printf("game_code=%s\n", h.game_code.c_str());
    std::printf("maker_code=%s\n", h.maker_code.c_str());
    std::printf("fixed_b2=0x%02x\n", h.fixed_b2);
    std::printf("main_unit_code=0x%02x\n", h.main_unit_code);
    std::printf("software_version=0x%02x\n", h.software_version);
    std::printf("complement_check=0x%02x\n", h.complement_check);
    std::printf("complement_expected=0x%02x\n", h.complement_expected);
    std::printf("complement_valid=%d\n", h.complement_valid ? 1 : 0);
    std::printf("logo_present=%d\n", h.logo_present ? 1 : 0);
    std::printf("save_type=%s\n", gba::save_type_name(h.save_type));
    std::printf("save_signature=%s\n", h.save_signature.c_str());
    std::printf("save_signature_offset=0x%08x\n", h.save_signature_offset);
    std::printf("ok=%d\n", h.ok ? 1 : 0);
    if (!h.ok) std::printf("error=%s\n", h.error.c_str());
    return h.ok ? 0 : 1;
}
