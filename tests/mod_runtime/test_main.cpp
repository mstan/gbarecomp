#include "mod_runtime.h"
#include "sha1.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool g_active = false;

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

void reset_view() {
    g_active = false;
    (void)gba_mod_set_adaptive_view_enabled(0);
}

void activate_view() {
    g_active = true;
    (void)gba_mod_set_adaptive_view_enabled(1);
}

bool write_text(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    return static_cast<bool>(file);
}

}  // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
    const fs::path sandbox = fs::temp_directory_path() /
        ("gbarecomp-mod-runtime-" + std::to_string(nonce));
    const fs::path mods = sandbox / "mods";
    const fs::path package = mods / "packages" /
        "test.adaptive-view" / "1.0.0";
    const fs::path rom_path = sandbox / "test.gba";
    std::error_code ec;
    fs::create_directories(package, ec);
    if (ec) return fail("could not create sandbox: " + ec.message());

    const std::vector<unsigned char> rom = {
        'g', 'b', 'a', 'r', 'e', 'c', 'o', 'm', 'p', '-', 'm', 'o', 'd'
    };
    {
        std::ofstream file(rom_path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(rom.data()),
                   static_cast<std::streamsize>(rom.size()));
        if (!file) return fail("could not write test ROM");
    }
    const std::string sha1 = gba::sha1(rom.data(), rom.size()).hex();
    const std::string manifest =
        "format_version = 1\n"
        "id = \"test.adaptive-view\"\n"
        "version = \"1.0.0\"\n"
        "name = \"Adaptive view test\"\n"
        "author = \"gbarecomp\"\n"
        "description = \"Trusted plugin vertical slice.\"\n"
        "license = \"MIT\"\n"
        "resolver = \"declarative\"\n"
        "save_compatibility = \"shared\"\n\n"
        "[[target]]\n"
        "game_id = \"test-game\"\n"
        "rom_sha1 = \"" + sha1 + "\"\n\n"
        "[[feature]]\n"
        "id = \"adaptive-view\"\n"
        "name = \"Adaptive view\"\n"
        "group = \"Display\"\n"
        "default_enabled = false\n\n"
        "[[plugin]]\n"
        "feature = \"adaptive-view\"\n"
        "id = \"test.adaptive-view\"\n";
    if (!write_text(package / "manifest.toml", manifest))
        return fail("could not write manifest");

    if (!gba_mod_register_reset_callback(reset_view) ||
        !gba_mod_register_activation_plugin(
            "test.adaptive-view", activate_view)) {
        return fail("could not register trusted plugin");
    }

    const auto write_state = [&](bool enabled) {
        return write_text(
            mods / "state.toml",
            "format_version = 1\n\n"
            "[[package]]\n"
            "id = \"test.adaptive-view\"\n"
            "version = \"1.0.0\"\n\n"
            "[[feature]]\n"
            "package_id = \"test.adaptive-view\"\n"
            "id = \"adaptive-view\"\n"
            "enabled = " + std::string(enabled ? "true\n" : "false\n"));
    };

    std::string error;
    if (!write_state(true) ||
        !gbarecomp::mod_runtime_initialize(
            mods, "test-game", sha1, &error) ||
        !gbarecomp::mod_runtime_commit(rom_path, &error)) {
        return fail("enabled plan failed: " + error);
    }
    gbarecomp::mod_runtime_activate_plugins();
    if (!g_active || !gba_mod_adaptive_view_enabled())
        return fail("enabled plugin did not activate");

    if (!write_state(false) ||
        !gbarecomp::mod_runtime_initialize(
            mods, "test-game", sha1, &error) ||
        !gbarecomp::mod_runtime_commit(rom_path, &error)) {
        return fail("disabled plan failed: " + error);
    }
    gbarecomp::mod_runtime_activate_plugins();
    if (g_active || gba_mod_adaptive_view_enabled())
        return fail("disabled plugin did not restore native view");

    const fs::path wrong_rom = sandbox / "wrong.gba";
    if (!write_text(wrong_rom, "wrong") ||
        gbarecomp::mod_runtime_commit(wrong_rom, &error)) {
        return fail("ROM identity guard accepted a mismatched image");
    }

    fs::remove_all(sandbox, ec);
    std::cout << "GBA mod runtime: target gate, persisted feature toggle, "
                 "trusted activation, and reset passed\n";
    return 0;
}
