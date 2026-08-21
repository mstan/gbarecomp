// config_overlay_smoke — pin the merge contract of load_config_overlay()
// (tools/gba_recompile/config.cpp).
//
// The overlay exists so a GENERATED config (a decomp symbol import) can
// compose with a game's HAND-AUTHORED game.toml instead of replacing it. The
// invariants that makes worth having:
//
//   * the base owns [program]; an overlay declaring one is an error, so a
//     generated file can never silently redefine the load address or entry;
//   * an overlay's [identity], when present, must match the base — this is
//     what binds imported addresses to the exact binary they came from;
//   * the base wins every conflict, so reviewed names and notes survive;
//   * a contradiction between the base's reviewed seeds and the overlay's
//     data layout is a hard error, not a silent drop.
//
// The test writes real TOML to temporary files because that is the actual
// interface: load_config()/load_config_overlay() take paths.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "config.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++g_failures;
    }
}

std::filesystem::path g_dir;

std::string write_toml(const char* name, const std::string& body) {
    const std::filesystem::path p = g_dir / name;
    std::ofstream out(p, std::ios::binary);
    out << body;
    out.close();
    return p.string();
}

// A minimal but complete base config. sha1 is never verified here —
// verify_identity() is a separate call against real bytes.
const char* kBaseToml = R"TOML(
[program]
name = "Test Program"
id = "TEST"
load_address = 0x08000000
size = 0x00400000
entry_pc = 0x08000000

[identity]
sha1 = "9d327c030c3e2d9007990518594f70c3340ac56f"

[[extra_func]]
addr = 0x08001000
mode = "thumb"
name = "reviewed_irq_callback"
note = "hand-authored: address-taken from the IRQ table"

[[data_range]]
start = 0x08300000
end = 0x08310000
note = "hand-authored data blob"

[[exclude_func]]
addr = 0x08002000
reason = "reviewed: misdecoded tail of a jump table"
)TOML";

gbarecomp::Config load_base() {
    gbarecomp::Config cfg;
    const std::string path = write_toml("base.toml", kBaseToml);
    check(gbarecomp::load_config(path, cfg), "base config loads");
    return cfg;
}

const gbarecomp::ConfigExtraFunc* find_func(const gbarecomp::Config& cfg,
                                             uint32_t addr) {
    for (const auto& ef : cfg.extra_funcs) {
        if (ef.addr == addr) return &ef;
    }
    return nullptr;
}

}  // namespace

int main() {
    g_dir = std::filesystem::temp_directory_path() /
            "gbarecomp_config_overlay_smoke";
    std::filesystem::create_directories(g_dir);

    // ── 1. A well-formed overlay appends, and the base keeps its name. ──
    {
        gbarecomp::Config cfg = load_base();
        const std::size_t base_funcs = cfg.extra_funcs.size();
        const std::size_t base_ranges = cfg.data_ranges.size();
        const std::string path = write_toml("ok.toml", R"TOML(
[identity]
sha1 = "9D327C030C3E2D9007990518594F70C3340AC56F"

[[extra_func]]
addr = 0x08001000
mode = "thumb"
name = "imported_name_that_must_lose"

[[extra_func]]
addr = 0x08004000
mode = "thumb"
name = "imported_new_function"

[[data_range]]
start = 0x08200000
end = 0x08210000
note = "non-executable (map)"

[[data_range]]
start = 0x08300000
end = 0x08310000
note = "duplicate of the base's range"
)TOML");
        check(gbarecomp::load_config_overlay(path, cfg),
              "overlay merges");
        // One new function; the conflicting one dropped.
        check(cfg.extra_funcs.size() == base_funcs + 1,
              "overlay added exactly one extra_func");
        const auto* kept = find_func(cfg, 0x08001000u);
        check(kept != nullptr && kept->name == "reviewed_irq_callback",
              "base wins the name conflict");
        check(kept != nullptr && !kept->note.empty(),
              "base keeps its reviewed note");
        check(find_func(cfg, 0x08004000u) != nullptr,
              "overlay's new function is present");
        // One new range; the exact duplicate dropped.
        check(cfg.data_ranges.size() == base_ranges + 1,
              "duplicate data_range dropped");
        // Identity comparison is case-insensitive (the overlay used caps).
    }

    // ── 2. [program] in an overlay is rejected. ──────────────────────
    {
        gbarecomp::Config cfg = load_base();
        const std::string path = write_toml("has_program.toml", R"TOML(
[program]
name = "Sneaky"
id = "SNEAK"
load_address = 0x02000000
size = 0x10
entry_pc = 0x02000000
)TOML");
        check(!gbarecomp::load_config_overlay(path, cfg),
              "overlay with [program] is rejected");
    }

    // ── 3. Mismatched identity is rejected. ──────────────────────────
    {
        gbarecomp::Config cfg = load_base();
        const std::string path = write_toml("wrong_id.toml", R"TOML(
[identity]
sha1 = "41cb23d8dccc8ebd7c649cd8fbb58eeace6e2fdc"

[[data_range]]
start = 0x08200000
end = 0x08210000
)TOML");
        check(!gbarecomp::load_config_overlay(path, cfg),
              "overlay from a different binary is rejected");
    }

    // ── 4. An overlay function the base excluded is dropped, not fatal. ─
    {
        gbarecomp::Config cfg = load_base();
        const std::size_t before = cfg.extra_funcs.size();
        const std::string path = write_toml("excluded.toml", R"TOML(
[[extra_func]]
addr = 0x08002000
mode = "thumb"
name = "decomp_thinks_this_is_a_function"
)TOML");
        check(gbarecomp::load_config_overlay(path, cfg),
              "overlay with an excluded address still loads");
        check(cfg.extra_funcs.size() == before,
              "excluded overlay function dropped");
    }

    // ── 5. An overlay function inside a base data_range is dropped. ──
    {
        gbarecomp::Config cfg = load_base();
        const std::size_t before = cfg.extra_funcs.size();
        const std::string path = write_toml("in_base_data.toml", R"TOML(
[[extra_func]]
addr = 0x08305000
mode = "thumb"
name = "inside_the_hand_authored_blob"
)TOML");
        check(gbarecomp::load_config_overlay(path, cfg),
              "overlay function inside base data still loads");
        check(cfg.extra_funcs.size() == before,
              "overlay function inside a base data_range dropped");
    }

    // ── 6. The reverse is a HARD error: an overlay data_range that
    //       swallows a reviewed base seed is a genuine contradiction. ──
    {
        gbarecomp::Config cfg = load_base();
        const std::string path = write_toml("swallows_base.toml", R"TOML(
[[data_range]]
start = 0x08000F00
end = 0x08001100
note = "non-executable (map)"
)TOML");
        check(!gbarecomp::load_config_overlay(path, cfg),
              "overlay data_range covering a base extra_func aborts");
    }

    // ── 7. Overlays compose: two of them, applied in order. ──────────
    {
        gbarecomp::Config cfg = load_base();
        const std::size_t base_ranges = cfg.data_ranges.size();
        const std::string a = write_toml("first.toml", R"TOML(
[[data_range]]
start = 0x08200000
end = 0x08210000
)TOML");
        const std::string b = write_toml("second.toml", R"TOML(
[[data_range]]
start = 0x08210000
end = 0x08220000
)TOML");
        check(gbarecomp::load_config_overlay(a, cfg), "first overlay loads");
        check(gbarecomp::load_config_overlay(b, cfg), "second overlay loads");
        check(cfg.data_ranges.size() == base_ranges + 2,
              "both overlays contributed");
    }

    // ── 8. A missing overlay file fails cleanly. ─────────────────────
    {
        gbarecomp::Config cfg = load_base();
        check(!gbarecomp::load_config_overlay(
                  (g_dir / "does_not_exist.toml").string(), cfg),
              "missing overlay file is an error");
    }

    std::error_code ec;
    std::filesystem::remove_all(g_dir, ec);

    if (g_failures != 0) {
        std::printf("config_overlay_smoke: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("config_overlay_smoke: OK\n");
    return 0;
}
